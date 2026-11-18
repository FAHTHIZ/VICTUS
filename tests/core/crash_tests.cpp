// SPDX-License-Identifier: Apache-2.0
// Crash reports and diagnostic sessions (victus/core/crash.hpp). The fatal paths (signals,
// std::terminate) are exercised end to end by tests/cli/diagnostics_smoke.cmake in a child
// process; these tests cover the report format, listing, session markers and log pruning.
#include "doctest.h"
#include "victus/core/crash.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace victus;
namespace sfs = std::filesystem;

namespace {
struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = sfs::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        sfs::remove_all(path, ec);
    }
};
void touch(const fs::Path& p, std::string_view body = "x\n") {
    sfs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
} // namespace

TEST_CASE("crash: session writes a log, a marker and readable reports") {
    TempDir tmp("victus_crash_session");
    crash::Options o;
    o.directory = tmp.path;
    o.app = "victus test/app"; // unsafe characters become '_'
    {
        auto s = crash::Session::start(o);
        REQUIRE(s.ok());
        CHECK_FALSE((*s)->previous_session_ended_unexpectedly());
        CHECK((*s)->directory() == tmp.path);
        REQUIRE_FALSE((*s)->log_path().empty());
        CHECK((*s)->log_path().filename().string().starts_with("victus_test_app-"));
        int markers = 0;
        for (const auto& e : sfs::directory_iterator(tmp.path / "sessions")) markers += e.path().extension() == ".running";
        CHECK(markers == 1);

        VICTUS_LOG_INFO("crash_test", "breadcrumb {}", 1234);
        auto report = crash::write_report("manual test report");
        REQUIRE(report.ok());
        auto body = fs::read_text(*report);
        REQUIRE(body.ok());
        CHECK(body->starts_with("VICTUS crash report\nformat: 1\n"));
        CHECK(body->find("reason: manual test report\n") != std::string::npos);
        CHECK(body->find("app: victus_test_app\n") != std::string::npos);
        CHECK(body->find("--- backtrace ---") != std::string::npos);
        CHECK(body->find("--- recent log ---") != std::string::npos);
        CHECK(body->find("breadcrumb 1234") != std::string::npos);

        const auto reports = crash::list_reports(tmp.path);
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].app == "victus_test_app");
        CHECK(reports[0].reason == "manual test report");
        CHECK(reports[0].unix_time > 0);

        // The session log is written line by line, so it is complete even if we crash now.
        auto log = fs::read_text((*s)->log_path());
        REQUIRE(log.ok());
        CHECK(log->find("breadcrumb 1234") != std::string::npos);
    }
    // A clean exit removes the marker.
    int markers = 0;
    for (const auto& e : sfs::directory_iterator(tmp.path / "sessions")) markers += e.path().extension() == ".running";
    CHECK(markers == 0);
}

TEST_CASE("crash: a stale marker means the previous session ended unexpectedly") {
    TempDir tmp("victus_crash_stale");
    touch(tmp.path / "sessions" / "victus_stale-999999999.running");
    touch(tmp.path / "sessions" / "other_app-999999999.running"); // other apps are not ours to judge
    crash::Options o;
    o.directory = tmp.path;
    o.app = "victus_stale";
    o.log_file = false;
    {
        auto s = crash::Session::start(o);
        REQUIRE(s.ok());
        CHECK((*s)->previous_session_ended_unexpectedly());
        CHECK((*s)->log_path().empty());
    }
    CHECK_FALSE(sfs::exists(tmp.path / "sessions" / "victus_stale-999999999.running"));
    CHECK(sfs::exists(tmp.path / "sessions" / "other_app-999999999.running"));
    auto again = crash::Session::start(o);
    REQUIRE(again.ok());
    CHECK_FALSE((*again)->previous_session_ended_unexpectedly());
}

TEST_CASE("crash: old logs and reports are pruned, newest kept") {
    TempDir tmp("victus_crash_prune");
    for (int i = 0; i < 6; ++i) {
        touch(tmp.path / "logs" / ("victus_prune-" + std::to_string(1000 + i) + "-1.log"));
        touch(tmp.path / "crashes" / ("crash-" + std::to_string(1000 + i) + "-1-victus_prune.txt"), "reason: old " + std::to_string(i) + "\n");
    }
    touch(tmp.path / "logs" / "victus_other-1000-1.log"); // another app's log is left alone
    touch(tmp.path / "crashes" / "notes.txt");             // unrelated files are ignored
    crash::Options o;
    o.directory = tmp.path;
    o.app = "victus_prune";
    o.keep_logs = 2;
    o.keep_crashes = 3;
    auto s = crash::Session::start(o);
    REQUIRE(s.ok());
    int logs = 0;
    for (const auto& e : sfs::directory_iterator(tmp.path / "logs"))
        logs += e.path().filename().string().starts_with("victus_prune-");
    CHECK(logs == 2); // the new session log plus the newest old one
    CHECK(sfs::exists(tmp.path / "logs" / "victus_prune-1005-1.log"));
    CHECK(sfs::exists(tmp.path / "logs" / "victus_other-1000-1.log"));
    const auto reports = crash::list_reports(tmp.path);
    REQUIRE(reports.size() == 3);
    CHECK(reports[0].reason == "old 5");
    CHECK(reports[2].reason == "old 3");
    CHECK(sfs::exists(tmp.path / "crashes" / "notes.txt"));
}

TEST_CASE("crash: trigger refuses unknown kinds") {
    CHECK_FALSE(crash::trigger("nothing"));
    if (!std::getenv("VICTUS_DIAGNOSTICS_DIR")) CHECK(crash::default_directory().filename() == "diagnostics");
}
