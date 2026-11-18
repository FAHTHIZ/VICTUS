// SPDX-License-Identifier: Apache-2.0
// Crash reports and diagnostic sessions (ADR-0072, docs/DIAGNOSTICS.md).
//
// A diagnostics directory (default `fs::user_data_dir()/diagnostics`, overridable with the
// VICTUS_DIAGNOSTICS_DIR environment variable) holds:
//   crashes/crash-<unix seconds>-<pid>-<app>.txt   one text report per fatal error
//   logs/<app>-<unix seconds>-<pid>.log            the log of each session (newest kept)
//   sessions/<app>.running                          present while a session runs
//
// A crash report is plain text: reason, fault address, version and build, platform, time,
// uptime, a backtrace with module path, module-relative offset and (Linux) GNU build id per frame
// so a matching symbol file can be found later (tools/crash/symbolize.py), and the last log lines.
// Nothing is ever uploaded: reports stay on disk until the user chooses to share a bundle.
//
// The fatal-signal path avoids heap allocation and locks: the report is formatted into static
// buffers and written with write(2). Backtraces use the platform unwinder (execinfo on glibc and
// macOS, CaptureStackBackTrace on Windows) and are best effort; Android reports carry no frames.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace victus::log {
class Sink;
}

namespace victus::crash {

// VICTUS_DIAGNOSTICS_DIR if set, else fs::user_data_dir()/diagnostics.
[[nodiscard]] fs::Path default_directory();

struct Options {
    fs::Path directory;       // empty = default_directory()
    std::string app{"victus"}; // file-name safe tag: victus, victus_player, victus_editor, ...
    bool log_file{true};      // write logs/<app>-<time>-<pid>.log through the process logger
    std::size_t keep_logs{5}; // session logs kept per app (older ones are deleted)
    std::size_t keep_crashes{20}; // crash reports kept (oldest deleted at session start)
};

// One diagnostic session per process: installs the fatal-signal/terminate handlers, adds the
// crash breadcrumb sink (last log lines kept for reports) and the session log file, and marks the
// session as running. Destroying it (normal exit) removes the marker and the handlers stay. If
// the previous session of the same app left its marker behind it ended abnormally (crash, kill,
// power loss); previous_session_ended_unexpectedly() reports that once.
class Session {
public:
    [[nodiscard]] static Result<std::unique_ptr<Session>> start(Options options);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] const fs::Path& directory() const noexcept { return dir_; }
    [[nodiscard]] const fs::Path& log_path() const noexcept { return log_path_; }
    [[nodiscard]] bool previous_session_ended_unexpectedly() const noexcept { return previous_unclean_; }

private:
    Session() = default;
    fs::Path dir_, log_path_, marker_;
    std::shared_ptr<log::Sink> file_sink_, crumbs_;
    bool previous_unclean_{false};
};

// Installs only the fatal handlers (no log file, no marker); reports go to `directory`/crashes.
// Safe to call again (the last directory wins). Also used by the legacy install_crash_handlers.
void install_handlers(const fs::Path& directory, std::string_view app);

// Writes a report for a non-fatal reason from normal code (for tests and "report a problem"):
// same format as a crash, current thread's stack. Returns the report path.
[[nodiscard]] Result<fs::Path> write_report(std::string_view reason);

// Deliberately crashes the process the way `kind` names, for testing the handlers end to end:
// "segv" (null write), "abort", "terminate" (uncaught exception), "assert" (VICTUS_VERIFY).
// Returns false for an unknown kind.
bool trigger(std::string_view kind);

struct ReportInfo {
    fs::Path path;
    std::string app, reason;
    std::int64_t unix_time{0};
};
// Reports in `directory`/crashes, newest first.
[[nodiscard]] std::vector<ReportInfo> list_reports(const fs::Path& directory);

} // namespace victus::crash
