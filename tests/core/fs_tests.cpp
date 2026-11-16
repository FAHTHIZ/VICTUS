// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/fs.hpp"

#include <filesystem>

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
