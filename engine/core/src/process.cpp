// SPDX-License-Identifier: Apache-2.0
#include "victus/core/process.hpp"

#include <chrono>
#include <cstring>
#include <format>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace victus::process {

namespace {
using Clock = std::chrono::steady_clock;
int remaining_ms(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

bool has_directory(std::string_view p) { return p.find('/') != std::string_view::npos || p.find('\\') != std::string_view::npos; }

#if defined(_WIN32)
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
// Quoting rules of CommandLineToArgvW / the MSVC CRT.
void append_quoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') ++i, ++backslashes;
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += L'"';
        } else {
            cmd.append(backslashes, L'\\');
            cmd += arg[i];
        }
    }
    cmd += L'"';
}
std::string last_error_text() { return std::format("Win32 error {}", static_cast<unsigned long>(GetLastError())); }
#endif
} // namespace

Result<std::string> find_executable(std::string_view name) {
    if (name.empty()) return make_error(ErrorCode::InvalidArgument, "empty executable name");
#if defined(_WIN32)
    std::vector<std::string> exts{""};
    if (const char* pe = std::getenv("PATHEXT")) {
        std::string_view s(pe);
        while (!s.empty()) {
            const auto c = s.find(';');
            exts.emplace_back(s.substr(0, c));
            if (c == std::string_view::npos) break;
            s.remove_prefix(c + 1);
        }
    } else {
        exts.insert(exts.end(), {".exe", ".bat", ".cmd"});
    }
    auto exists = [](const std::string& p) {
        const DWORD a = GetFileAttributesW(widen(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    const char sep = ';';
#else
    const std::vector<std::string> exts{""};
    auto exists = [](const std::string& p) {
        struct stat st{};
        return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
    };
    const char sep = ':';
#endif
    if (has_directory(name)) {
        for (const auto& e : exts)
            if (exists(std::string(name) + e)) return std::string(name) + e;
        return make_error(ErrorCode::NotFound, std::format("executable '{}' not found", name));
    }
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "";
    while (!rest.empty()) {
        const auto c = rest.find(sep);
        std::string dir(rest.substr(0, c));
        if (!dir.empty()) {
            for (const auto& e : exts) {
                std::string candidate = dir + "/" + std::string(name) + e;
                if (exists(candidate)) return candidate;
            }
        }
        if (c == std::string_view::npos) break;
        rest.remove_prefix(c + 1);
    }
    return make_error(ErrorCode::NotFound, std::format("executable '{}' not found on PATH", name));
}

Status ChildProcess::write_line(std::string_view line) {
    std::string s;
    s.reserve(line.size() + 1);
    s.append(line);
    s.push_back('\n');
    return write(s);
}

Result<std::optional<std::string>> ChildProcess::read_line(int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    std::size_t scanned = 0;
    for (;;) {
        const auto nl = buffer_.find('\n', scanned);
        if (nl != std::string::npos) {
            // The whole over-long line may arrive in one read: enforce the limit on it too.
            if (nl > max_line_)
                return make_error(ErrorCode::LimitExceeded, std::format("child output line exceeds {} bytes", max_line_));
            std::string line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return std::optional<std::string>(std::move(line));
        }
        scanned = buffer_.size();
        if (buffer_.size() > max_line_)
            return make_error(ErrorCode::LimitExceeded, std::format("child output line exceeds {} bytes", max_line_));
        if (eof_) {
            std::string partial = std::move(buffer_);
            buffer_.clear();
            return make_error(ErrorCode::IoError, partial.empty() ? std::string("child closed its output")
                                                                  : "child closed its output mid-line: " + partial.substr(0, 200));
        }
        const int wait = timeout_ms < 0 ? -1 : remaining_ms(deadline);
        Status err;
        if (!fill(wait, err)) {
            if (!err) return std::move(err).error();
            if (timeout_ms >= 0 && Clock::now() >= deadline) return std::optional<std::string>{};
        }
    }
}

std::optional<int> ChildProcess::wait(int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (!running()) return exit_code_;
        if (timeout_ms >= 0 && Clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

#if defined(_WIN32)

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const LaunchOptions& o) {
    if (o.args.empty()) return make_error(ErrorCode::InvalidArgument, "no program given");
    auto program = find_executable(o.args[0]);
    if (!program) return std::move(program).error();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0))
        return make_error(ErrorCode::IoError, "CreatePipe failed: " + last_error_text());
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE err = o.discard_stderr ? CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr)
                                  : GetStdHandle(STD_ERROR_HANDLE);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err;
    std::wstring cmd;
    for (std::size_t i = 0; i < o.args.size(); ++i) {
        if (i) cmd += L' ';
        append_quoted(cmd, widen(i == 0 ? *program : o.args[i]));
    }
    const std::wstring app = widen(*program), cwd = widen(o.working_directory);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (o.discard_stderr && err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        return make_error(ErrorCode::IoError, std::format("could not start '{}': {}", *program, last_error_text()));
    }
    CloseHandle(pi.hThread);
    std::unique_ptr<ChildProcess> c(new ChildProcess());
    c->process_ = pi.hProcess;
    c->pid_ = pi.dwProcessId;
    c->stdin_write_ = in_w;
    c->stdout_read_ = out_r;
    c->max_line_ = o.max_line_bytes;
    return c;
}

ChildProcess::~ChildProcess() {
    close_stdin();
    if (!wait(1000)) kill();
    if (stdout_read_) CloseHandle(stdout_read_);
    if (process_) CloseHandle(process_);
}

Status ChildProcess::write(std::string_view data) {
    if (!stdin_write_) return make_error(ErrorCode::IoError, "child stdin is closed");
    while (!data.empty()) {
        DWORD n = 0;
        if (!WriteFile(stdin_write_, data.data(), static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 20)), &n, nullptr))
            return make_error(ErrorCode::IoError, "write to child failed: " + last_error_text());
        data.remove_prefix(n);
    }
    return {};
}

void ChildProcess::close_stdin() noexcept {
    if (stdin_write_) CloseHandle(stdin_write_);
    stdin_write_ = nullptr;
}

bool ChildProcess::fill(int timeout_ms, Status& error) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &avail, nullptr)) {
            eof_ = true; // broken pipe: the child closed its end
            return true;
        }
        if (avail > 0) {
            char buf[65536];
            DWORD n = 0;
            if (!ReadFile(stdout_read_, buf, std::min<DWORD>(avail, sizeof buf), &n, nullptr) || n == 0) {
                eof_ = true;
                return true;
            }
            buffer_.append(buf, n);
            return true;
        }
        if (timeout_ms >= 0 && Clock::now() >= deadline) return false;
        Sleep(1);
    }
    (void)error;
}

bool ChildProcess::running() {
    if (exit_code_) return false;
    DWORD code = 0;
    if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(process_, &code)) {
        exit_code_ = static_cast<int>(code);
        return false;
    }
    return true;
}

std::optional<int> ChildProcess::exit_code() {
    (void)running();
    return exit_code_;
}

void ChildProcess::kill() noexcept {
    if (process_ && !exit_code_) {
        TerminateProcess(process_, 137);
        WaitForSingleObject(process_, 2000);
        (void)running();
    }
}

#else // POSIX

Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const LaunchOptions& o) {
    if (o.args.empty()) return make_error(ErrorCode::InvalidArgument, "no program given");
    auto program = find_executable(o.args[0]);
    if (!program) return std::move(program).error();
    int in_pipe[2], out_pipe[2];
    if (::pipe(in_pipe) != 0) return make_error(ErrorCode::IoError, std::format("pipe: {}", std::strerror(errno)));
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return make_error(ErrorCode::IoError, std::format("pipe: {}", std::strerror(errno)));
    }
    // The parent's ends must not leak into this or any later child.
    ::fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
    if (o.discard_stderr) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
#if defined(__GLIBC__) && ((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    if (!o.working_directory.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.working_directory.c_str());
#elif defined(__APPLE__)
    // Available since macOS 10.15; newer SDKs mark the _np name deprecated in favour of
    // posix_spawn_file_actions_addchdir, which older deployment targets do not have.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!o.working_directory.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.working_directory.c_str());
#pragma clang diagnostic pop
#else
    if (!o.working_directory.empty()) {
        posix_spawn_file_actions_destroy(&fa);
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) ::close(fd);
        return make_error(ErrorCode::Unsupported, "working_directory needs posix_spawn_file_actions_addchdir_np");
    }
#endif
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program->c_str()));
    for (std::size_t i = 1; i < o.args.size(); ++i) argv.push_back(const_cast<char*>(o.args[i].c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, program->c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        return make_error(ErrorCode::IoError, std::format("could not start '{}': {}", *program, std::strerror(rc)));
    }
    std::unique_ptr<ChildProcess> c(new ChildProcess());
    c->pid_ = static_cast<std::uint64_t>(pid);
    c->stdin_fd_ = in_pipe[1];
    c->stdout_fd_ = out_pipe[0];
    c->max_line_ = o.max_line_bytes;
    return c;
}

ChildProcess::~ChildProcess() {
    close_stdin();
    if (stdout_fd_ >= 0) ::close(stdout_fd_); // a child blocked writing output now gets EPIPE
    stdout_fd_ = -1;
    if (!wait(1000)) {
        kill();
    }
}

Status ChildProcess::write(std::string_view data) {
    if (stdin_fd_ < 0) return make_error(ErrorCode::IoError, "child stdin is closed");
    // Writing to a pipe whose reader exited raises SIGPIPE; block it on this thread and consume
    // any pending instance so a dead child is reported as an error instead of killing us.
#if defined(__APPLE__)
    // macOS has no sigtimedwait; F_SETNOSIGPIPE makes this pipe report EPIPE instead.
    (void)::fcntl(stdin_fd_, F_SETNOSIGPIPE, 1);
#else
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, &old);
#endif
    Status result;
    while (!data.empty()) {
        const ssize_t n = ::write(stdin_fd_, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            result = make_error(ErrorCode::IoError, std::format("write to child failed: {}", std::strerror(errno)));
            break;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
#if !defined(__APPLE__)
    if (!result) {
        const timespec zero{0, 0};
        while (sigtimedwait(&block, nullptr, &zero) > 0) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
#endif
    return result;
}

void ChildProcess::close_stdin() noexcept {
    if (stdin_fd_ >= 0) ::close(stdin_fd_);
    stdin_fd_ = -1;
}

bool ChildProcess::fill(int timeout_ms, Status& error) {
    if (stdout_fd_ < 0) {
        eof_ = true;
        return true;
    }
    pollfd p{stdout_fd_, POLLIN, 0};
    int r;
    do r = ::poll(&p, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
    if (r < 0) {
        error = make_error(ErrorCode::IoError, std::format("poll: {}", std::strerror(errno)));
        return false;
    }
    if (r == 0) return false;
    char buf[65536];
    ssize_t n;
    do n = ::read(stdout_fd_, buf, sizeof buf);
    while (n < 0 && errno == EINTR);
    if (n <= 0) {
        eof_ = true;
        return true;
    }
    buffer_.append(buf, static_cast<std::size_t>(n));
    return true;
}

bool ChildProcess::running() {
    if (exit_code_) return false;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == static_cast<pid_t>(pid_)) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        return false;
    }
    if (r < 0 && errno == ECHILD) {
        exit_code_ = -1;
        return false;
    }
    return true;
}

std::optional<int> ChildProcess::exit_code() {
    (void)running();
    return exit_code_;
}

void ChildProcess::kill() noexcept {
    if (pid_ && !exit_code_) {
        ::kill(static_cast<pid_t>(pid_), SIGKILL);
        int status = 0;
        if (::waitpid(static_cast<pid_t>(pid_), &status, 0) == static_cast<pid_t>(pid_))
            exit_code_ = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
    }
}

#endif

} // namespace victus::process
