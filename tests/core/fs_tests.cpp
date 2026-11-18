// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/fs.hpp"

#include <filesystem>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace victus;

TEST_CASE("fs: atomic write and read back") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_fs_test";
    std::filesystem::remove_all(dir);
    const auto file = dir / "nested" / "a.txt";
    REQUIRE(fs::write_text_atomic(file, "hello").ok());
    auto text = fs::read_text(file);
    REQUIRE(text.ok());
    CHECK(*text == "hello");
    REQUIRE(fs::write_text_atomic(file, "replaced").ok());
    CHECK(*fs::read_text(file) == "replaced");
    int leftovers = 0;
    for (auto& e : std::filesystem::directory_iterator(file.parent_path())) leftovers += e.path().filename() != "a.txt";
    CHECK(leftovers == 0);
    CHECK(fs::read_text(file, 3).error().code == ErrorCode::LimitExceeded);
    CHECK(fs::read_text(dir / "missing").error().code == ErrorCode::NotFound);
    std::filesystem::remove_all(dir);
}

TEST_CASE("fs: relative path sanitization rejects escapes") {
    CHECK(*fs::sanitize_relative("assets\\textures/./hero.png") == "assets/textures/hero.png");
    CHECK(*fs::sanitize_relative("a//b/") == "a/b");
    CHECK_FALSE(fs::sanitize_relative("../secret").ok());
    CHECK_FALSE(fs::sanitize_relative("a/../../b").ok());
    CHECK_FALSE(fs::sanitize_relative("/etc/passwd").ok());
    CHECK_FALSE(fs::sanitize_relative("C:/Windows").ok());
    CHECK_FALSE(fs::sanitize_relative("").ok());
    CHECK_FALSE(fs::sanitize_relative(".").ok());
}

TEST_CASE("fs: executable and user dirs resolve") {
    CHECK_FALSE(fs::executable_path().empty());
    CHECK_FALSE(fs::user_data_dir().empty());
}

namespace {

std::string slurp(const std::filesystem::path& p) {
    auto t = fs::read_text(p);
    return t ? *t : std::string("<missing>");
}

// "ok:<n>" documents are valid; "new:<n>" is a readable file from a newer engine.
Status check_doc(std::string_view text) {
    if (text.starts_with("ok:")) return {};
    if (text.starts_with("new:")) return make_error(ErrorCode::VersionMismatch, "newer format");
    return make_error(ErrorCode::ParseError, "damaged");
}

int temp_files(const std::filesystem::path& dir) {
    int n = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) n += e.path().filename().string().find(".tmp") != std::string::npos;
    return n;
}

} // namespace

TEST_CASE("fs: atomic writes surface write faults and never touch the target") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_fs_fault_test";
    std::filesystem::remove_all(dir);
    const auto file = dir / "a.txt";
    REQUIRE(fs::write_text_atomic(file, "good").ok());
    fs::testing::inject_write_fault(fs::testing::WriteFault::NoSpace);
    auto st = fs::write_text_atomic(file, "new");
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().message.find("No space left") != std::string::npos);
    CHECK(slurp(file) == "good");
    fs::testing::inject_write_fault(fs::testing::WriteFault::CorruptTemp);
    st = fs::write_text_atomic(file, "new");
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().message.find("verification failed") != std::string::npos);
    CHECK(slurp(file) == "good");
    CHECK(temp_files(dir) == 0);
    REQUIRE(fs::write_text_atomic(file, "").ok()); // empty files are fine
    CHECK(slurp(file).empty());
#if !defined(_WIN32)
    // A read-only directory: the error names the file and the target is untouched.
    if (::geteuid() != 0) {
        REQUIRE(fs::write_text_atomic(file, "good").ok());
        std::filesystem::permissions(dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
        st = fs::write_text_atomic(file, "new");
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
        REQUIRE_FALSE(st.ok());
        CHECK(slurp(file) == "good");
    }
#endif
    std::filesystem::remove_all(dir);
}

TEST_CASE("fs: safe writes keep verified generations and recover from damage") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_fs_safe_test";
    std::filesystem::remove_all(dir);
    const auto file = dir / "level.vscene";
    const fs::SafeWriteOptions opts{.history = 2, .validate = check_doc};
    CHECK(fs::history_path(file, 1).filename() == ".level.vscene.vbak1");

    // Invalid new bytes are refused before anything is written.
    auto st = fs::write_safely(file, "garbage", opts);
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().message.find("refusing to save") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(file));

    for (int i = 1; i <= 4; ++i) REQUIRE(fs::write_safely(file, "ok:" + std::to_string(i), opts).ok());
    CHECK(slurp(file) == "ok:4");
    CHECK(slurp(fs::history_path(file, 1)) == "ok:3");
    CHECK(slurp(fs::history_path(file, 2)) == "ok:2");
    CHECK_FALSE(std::filesystem::exists(fs::history_path(file, 3))); // oldest dropped
    CHECK(fs::generations(file) == std::vector<std::uint32_t>{1, 2});
    CHECK(temp_files(dir) == 0);

    // Interrupted after the history rotated: the current file is still the complete old one.
    fs::testing::inject_write_fault(fs::testing::WriteFault::CrashBeforeSwap);
    REQUIRE_FALSE(fs::write_safely(file, "ok:5", opts).ok());
    CHECK(slurp(file) == "ok:4");
    CHECK(slurp(fs::history_path(file, 1)) == "ok:4");
    // Disk full: nothing changes.
    fs::testing::inject_write_fault(fs::testing::WriteFault::NoSpace);
    REQUIRE_FALSE(fs::write_safely(file, "ok:6", opts).ok());
    CHECK(slurp(file) == "ok:4");

    // A torn current file is read from the newest valid generation...
    REQUIRE(fs::write_text_atomic(file, "ok:7 but torn").ok());
    std::filesystem::resize_file(file, 2); // "ok" without the colon -> damaged
    auto rec = fs::read_with_recovery(file, check_doc);
    REQUIRE(rec.ok());
    CHECK(rec->generation == 1);
    CHECK(rec->bytes == "ok:4");
    REQUIRE(rec->rejected.size() == 1);
    CHECK(rec->rejected[0].find("level.vscene") != std::string::npos);
    // ...and the next save preserves it as .damaged instead of rotating it into the history.
    REQUIRE(fs::write_safely(file, "ok:8", opts).ok());
    CHECK(slurp(fs::damaged_path(file)) == "ok");
    CHECK(slurp(fs::history_path(file, 1)) == "ok:4");
    CHECK(slurp(file) == "ok:8");

    // Missing current file (deleted mid-rename by an external tool): generation 1 is used.
    std::filesystem::remove(file);
    rec = fs::read_with_recovery(file, check_doc);
    REQUIRE(rec.ok());
    CHECK(rec->bytes == "ok:4");

    // Restoring a generation keeps the replaced file as generation 1 (undoable).
    REQUIRE(fs::write_safely(file, "ok:9", opts).ok());
    // (ok:9 replaced a missing file, so the history is still gen 1 = ok:4, gen 2 = ok:3.)
    REQUIRE(fs::restore_generation(file, 2, opts).ok());
    CHECK(slurp(file) == "ok:3");
    CHECK(slurp(fs::history_path(file, 1)) == "ok:9");
    CHECK_FALSE(fs::restore_generation(file, 7, opts).ok());

    // A newer-engine file is never replaced by an older generation, and is kept when saving over it.
    REQUIRE(fs::write_text_atomic(file, "new:1").ok());
    rec = fs::read_with_recovery(file, check_doc);
    REQUIRE_FALSE(rec.ok());
    CHECK(rec.error().code == ErrorCode::VersionMismatch);
    REQUIRE(fs::write_safely(file, "ok:10", opts).ok());
    CHECK(slurp(fs::history_path(file, 1)) == "new:1");

    // Nothing valid anywhere: an error naming every rejected copy; no file at all: NotFound.
    for (std::uint32_t g = 0; g <= 2; ++g) REQUIRE(fs::write_text_atomic(g ? fs::history_path(file, g) : file, "bad").ok());
    rec = fs::read_with_recovery(file, check_doc);
    REQUIRE_FALSE(rec.ok());
    CHECK(rec.error().message.find(".level.vscene.vbak2") != std::string::npos);
    CHECK(fs::read_with_recovery(dir / "nope.vscene", check_doc).error().code == ErrorCode::NotFound);
    // Without history nothing is rotated.
    const auto plain = dir / "plain.txt";
    REQUIRE(fs::write_safely(plain, "a", {.history = 0}).ok());
    REQUIRE(fs::write_safely(plain, "b", {.history = 0}).ok());
    CHECK(fs::generations(plain).empty());
    // Hidden history files stay out of listings.
    for (const auto& f : fs::list_files(dir)) CHECK(f.front() != '.');
    std::filesystem::remove_all(dir);
}
