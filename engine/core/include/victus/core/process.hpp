// SPDX-License-Identifier: Apache-2.0
// Child processes with piped standard streams: tools, out-of-process simulations, language
// servers and line-based bridges (one JSON object per line) launched by games, the editor or the
// CLI. stdin and stdout are pipes owned by the ChildProcess; stderr is inherited (diagnostics
// go to the parent's console/log) unless `discard_stderr` is set.
//
// POSIX uses posix_spawnp + poll(); Windows uses CreateProcessW with anonymous pipes and
// PeekNamedPipe for timed reads. Arguments are passed verbatim (no shell).
#pragma once
#include "victus/core/result.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::process {

struct LaunchOptions {
    std::vector<std::string> args; // args[0] is the program; looked up on PATH when it has no directory
    std::string working_directory; // empty: inherit
    bool discard_stderr{false};
    std::size_t max_line_bytes{512u * 1024u * 1024u}; // read_line fails beyond this (untrusted output)
};

class ChildProcess {
public:
    [[nodiscard]] static Result<std::unique_ptr<ChildProcess>> spawn(const LaunchOptions& options);
    ~ChildProcess(); // closes stdin, waits up to one second, then kills
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Writes all bytes to the child's stdin (blocking).
    [[nodiscard]] Status write(std::string_view data);
    [[nodiscard]] Status write_line(std::string_view line);
    // Closes the child's stdin (EOF for the child). Further writes fail.
    void close_stdin() noexcept;
    // Next line from the child's stdout without the trailing '\n' (and '\r'). Waits at most
    // `timeout_ms` (0 = poll, <0 = forever); returns nullopt on timeout. End of stream before a
    // complete line is an error (IoError) carrying any partial text in the message.
    [[nodiscard]] Result<std::optional<std::string>> read_line(int timeout_ms = -1);
    // Exactly `count` bytes of stdout (binary protocols after a header line), same timeout and
    // end-of-stream rules as read_line.
    [[nodiscard]] Result<std::optional<std::string>> read_bytes(std::size_t count, int timeout_ms = -1);

    // True until the child has exited (non-blocking check).
    [[nodiscard]] bool running();
    // Exit code once the child has exited (signals map to 128 + signal on POSIX).
    [[nodiscard]] std::optional<int> exit_code();
    // Blocks until exit (up to timeout_ms, <0 forever); returns the exit code or nullopt.
    [[nodiscard]] std::optional<int> wait(int timeout_ms = -1);
    void kill() noexcept;
    [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

private:
    ChildProcess() = default;
    bool fill(int timeout_ms, Status& error); // reads more stdout into buffer_
    std::uint64_t pid_{0};
    std::optional<int> exit_code_;
    std::string buffer_;
    std::size_t max_line_{0};
    bool eof_{false};
#if defined(_WIN32)
    void* process_{nullptr};
    void* stdin_write_{nullptr};
    void* stdout_read_{nullptr};
#else
    int stdin_fd_{-1};
    int stdout_fd_{-1};
#endif
};

// Full path of an executable found on PATH (with PATHEXT on Windows), or NotFound.
[[nodiscard]] Result<std::string> find_executable(std::string_view name);

} // namespace victus::process
