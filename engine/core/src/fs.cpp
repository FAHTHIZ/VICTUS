// SPDX-License-Identifier: Apache-2.0
#include "victus/core/fs.hpp"

#include "victus/core/log.hpp"
#include "victus/core/pack.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <format>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <random>
#include <set>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace victus::fs {

namespace {

struct Mount {
    Path root; // absolute, lexically normal
    std::shared_ptr<const pack::Pack> pack;
};

std::mutex g_mount_mutex;
std::vector<Mount> g_mounts;

Path normal_abs(const Path& p) {
    std::error_code ec;
    Path a = std::filesystem::absolute(p, ec);
    if (ec) a = p;
    a = a.lexically_normal();
    // "dir/sub/.." normalizes to "dir/"; drop the trailing separator so it compares equal to "dir".
    if (!a.has_filename() && a.has_relative_path()) a = a.parent_path();
    return a;
}

// Relative '/' path of `p` under `root`, or empty when outside (or equal to) root.
std::string relative_under(const Path& p, const Path& root) {
    const Path rel = p.lexically_relative(root);
    if (rel.empty()) return {};
    std::string s = rel.generic_string();
    if (s == "." || s.starts_with("..")) return {};
    if (s.ends_with("/.")) s.resize(s.size() - 2);
    return s;
}

std::vector<Mount> mounts_snapshot() {
    std::lock_guard lock(g_mount_mutex);
    return g_mounts;
}

} // namespace

void mount(const Path& root, std::shared_ptr<const pack::Pack> pack) {
    const Path r = normal_abs(root);
    std::lock_guard lock(g_mount_mutex);
    std::erase_if(g_mounts, [&](const Mount& m) { return m.root == r; });
    if (pack) g_mounts.push_back({r, std::move(pack)});
}

void unmount(const Path& root) { mount(root, nullptr); }

void unmount_all() {
    std::lock_guard lock(g_mount_mutex);
    g_mounts.clear();
}

bool is_mounted(const Path& root) {
    const Path r = normal_abs(root);
    std::lock_guard lock(g_mount_mutex);
    return std::any_of(g_mounts.begin(), g_mounts.end(), [&](const Mount& m) { return m.root == r; });
}

bool is_virtual(const Path& path) {
    const auto mounts = mounts_snapshot();
    if (mounts.empty()) return false;
    const Path p = normal_abs(path);
    for (const Mount& m : mounts) {
        const std::string rel = relative_under(p, m.root);
        if (!rel.empty() && m.pack->contains(rel)) return true;
    }
    return false;
}

bool exists(const Path& path) {
    const auto mounts = mounts_snapshot();
    if (!mounts.empty()) {
        const Path p = normal_abs(path);
        for (const Mount& m : mounts) {
            const std::string rel = relative_under(p, m.root);
            if (!rel.empty() && m.pack->contains(rel)) return true;
        }
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::vector<std::string> list_files(const Path& dir) {
    std::set<std::string> out;
    const auto mounts = mounts_snapshot();
    if (!mounts.empty()) {
        const Path d = normal_abs(dir);
        for (const Mount& m : mounts) {
            if (d == m.root) {
                for (auto& e : m.pack->list()) out.insert(std::move(e));
                continue;
            }
            const std::string rel = relative_under(d, m.root);
            if (rel.empty()) continue;
            for (const auto& e : m.pack->list(rel)) out.insert(e.substr(rel.size() + 1));
        }
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) out.insert(std::filesystem::relative(it->path(), dir, ec).generic_string());
    }
    std::vector<std::string> v;
    for (const auto& s : out)
        if (!s.empty() && s[0] != '.' && s.find("/.") == std::string::npos) v.push_back(s);
    return v;
}

Result<std::vector<std::uint8_t>> read_binary(const Path& path, std::uint64_t max_bytes) {
    if (const auto mounts = mounts_snapshot(); !mounts.empty()) {
        const Path p = normal_abs(path);
        for (const Mount& m : mounts) {
            const std::string rel = relative_under(p, m.root);
            if (!rel.empty() && m.pack->contains(rel)) return m.pack->read(rel, max_bytes);
        }
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return make_error(ErrorCode::NotFound, std::format("cannot stat '{}': {}", path.string(), ec.message()));
    if (size > max_bytes)
        return make_error(ErrorCode::LimitExceeded, std::format("'{}' is {} bytes; limit is {}", path.string(), size, max_bytes));
    std::ifstream in(path, std::ios::binary);
    if (!in) return make_error(ErrorCode::IoError, std::format("cannot open '{}'", path.string()));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
        return make_error(ErrorCode::IoError, std::format("short read from '{}'", path.string()));
    return data;
}

Result<std::string> read_text(const Path& path, std::uint64_t max_bytes) {
    auto bytes = read_binary(path, max_bytes);
    if (!bytes) return std::move(bytes).error();
    return std::string(bytes->begin(), bytes->end());
}

namespace {

std::atomic<testing::WriteFault> g_fault{testing::WriteFault::None};

testing::WriteFault take_fault(testing::WriteFault only) noexcept {
    testing::WriteFault expected = only;
    return g_fault.compare_exchange_strong(expected, testing::WriteFault::None) ? only : testing::WriteFault::None;
}

Path hidden_sibling(const Path& path, std::string_view suffix) {
    Path out = path.parent_path();
    out /= "." + path.filename().string() + std::string(suffix);
    return out;
}

Path temp_sibling(const Path& path) {
    std::random_device rd;
    return hidden_sibling(path, std::format(".tmp{:08x}", rd()));
}

// Writes and flushes `bytes` to a new file; reports the OS reason (e.g. "No space left on device").
Status write_durable(const Path& file, std::span<const std::uint8_t> bytes) {
    if (take_fault(testing::WriteFault::NoSpace) == testing::WriteFault::NoSpace)
        return make_error(ErrorCode::IoError, std::format("write failed for '{}': No space left on device (injected)", file.string()));
#if defined(_WIN32)
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return make_error(ErrorCode::IoError, std::format("cannot create '{}' (error {})", file.string(), GetLastError()));
    std::size_t done = 0;
    bool ok = true;
    while (ok && done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 30));
        DWORD wrote = 0;
        ok = WriteFile(h, bytes.data() + done, chunk, &wrote, nullptr) && wrote == chunk;
        done += wrote;
    }
    const DWORD err = ok ? 0 : GetLastError();
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) return make_error(ErrorCode::IoError, std::format("write failed for '{}' (error {})", file.string(), err ? err : GetLastError()));
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return make_error(ErrorCode::IoError, std::format("cannot create '{}': {}", file.string(), std::strerror(errno)));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const int err = n < 0 ? errno : ENOSPC;
            ::close(fd);
            return make_error(ErrorCode::IoError, std::format("write failed for '{}': {}", file.string(), std::strerror(err)));
        }
        done += static_cast<std::size_t>(n);
    }
#if defined(__APPLE__)
    const bool synced = ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0; // F_FULLFSYNC reaches the platter
#else
    const bool synced = ::fsync(fd) == 0;
#endif
    const int err = errno;
    if (::close(fd) != 0 || !synced)
        return make_error(ErrorCode::IoError, std::format("cannot flush '{}': {}", file.string(), std::strerror(synced ? errno : err)));
#endif
    if (take_fault(testing::WriteFault::CorruptTemp) == testing::WriteFault::CorruptTemp && !bytes.empty()) {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x5a);
        f.seekp(0);
        f.write(&c, 1);
    }
    return {};
}

// Makes a rename durable: on POSIX the directory entry must be flushed too.
void sync_directory(const Path& dir) {
#if !defined(_WIN32)
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)::fsync(fd); // not supported on every file system; the rename is still atomic
        ::close(fd);
    }
#else
    (void)dir;
#endif
}

Status verify_file(const Path& file, std::span<const std::uint8_t> bytes) {
    auto back = read_binary(file, bytes.size() + 1);
    if (!back || back->size() != bytes.size() || !std::equal(bytes.begin(), bytes.end(), back->begin()))
        return make_error(ErrorCode::IoError, std::format("verification failed: '{}' does not read back as written", file.string()));
    return {};
}

// Writes a verified temporary next to `path` and returns its name.
Result<Path> stage(const Path& path, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    const Path tmp = temp_sibling(path);
    Status st = write_durable(tmp, bytes);
    if (st) st = verify_file(tmp, bytes);
    if (!st) {
        std::filesystem::remove(tmp, ec);
        return std::move(st).error();
    }
    return tmp;
}

Status swap_in(const Path& tmp, const Path& path) {
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        const std::string why = ec.message();
        std::filesystem::remove(tmp, ec);
        return make_error(ErrorCode::IoError, std::format("cannot replace '{}': {}", path.string(), why));
    }
    sync_directory(path.parent_path());
    return {};
}

std::span<const std::uint8_t> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

} // namespace

void testing::inject_write_fault(WriteFault fault) noexcept { g_fault.store(fault); }

Status write_atomic(const Path& path, std::span<const std::uint8_t> bytes) {
    auto tmp = stage(path, bytes);
    if (!tmp) return std::move(tmp).error();
    if (take_fault(testing::WriteFault::CrashBeforeSwap) == testing::WriteFault::CrashBeforeSwap)
        return make_error(ErrorCode::IoError, "interrupted before replacing (injected)");
    return swap_in(*tmp, path);
}

Status write_text_atomic(const Path& path, std::string_view text) { return write_atomic(path, as_bytes(text)); }

Path history_path(const Path& path, std::uint32_t generation) {
    return hidden_sibling(path, std::format(".vbak{}", generation));
}

Path damaged_path(const Path& path) { return hidden_sibling(path, ".damaged"); }

Status write_safely(const Path& path, std::string_view bytes, const SafeWriteOptions& options) {
    if (options.validate)
        if (Status st = options.validate(bytes); !st)
            return make_error(st.error().code, std::format("refusing to save '{}': {}", path.filename().string(), st.error().message));
    const std::uint32_t keep = std::min(options.history, kMaxHistory);
    auto tmp = stage(path, as_bytes(bytes));
    if (!tmp) return std::move(tmp).error();
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        auto current = read_text(path);
        Status ok = current ? Status{} : Status{current.error()};
        if (ok && options.validate) {
            ok = options.validate(*current);
            if (!ok && ok.error().code == ErrorCode::VersionMismatch) ok = {}; // newer, not damaged
        }
        if (!ok) {
            // Never overwrite or rotate a file we could not verify: keep it for inspection.
            std::filesystem::rename(path, damaged_path(path), ec);
            VICTUS_LOG_WARN("fs", "'{}' failed validation ({}); kept as '{}'", path.string(), ok.error().message,
                            damaged_path(path).filename().string());
        } else if (keep > 0) {
            std::filesystem::remove(history_path(path, keep), ec);
            for (std::uint32_t g = keep; g > 1; --g)
                if (std::filesystem::exists(history_path(path, g - 1), ec))
                    std::filesystem::rename(history_path(path, g - 1), history_path(path, g), ec);
            // Copy (not move) so `path` stays complete until the final rename.
            auto copy = stage(history_path(path, 1), as_bytes(*current));
            if (!copy) {
                std::filesystem::remove(*tmp, ec);
                return make_error(copy.error().code, std::format("cannot keep the previous '{}': {}", path.filename().string(),
                                                                 copy.error().message));
            }
            if (Status st = swap_in(*copy, history_path(path, 1)); !st) {
                std::filesystem::remove(*tmp, ec);
                return st;
            }
        }
    }
    if (take_fault(testing::WriteFault::CrashBeforeSwap) == testing::WriteFault::CrashBeforeSwap)
        return make_error(ErrorCode::IoError, "interrupted before replacing (injected)");
    return swap_in(*tmp, path);
}

Result<Recovered> read_with_recovery(const Path& path, const Validator& validate, std::uint32_t history,
                                     std::uint64_t max_bytes) {
    Recovered out;
    bool any = false;
    history = std::min(history, kMaxHistory);
    for (std::uint32_t g = 0; g <= history; ++g) {
        const Path file = g == 0 ? path : history_path(path, g);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec) && !(g == 0 && is_virtual(file))) continue;
        any = true;
        auto text = read_text(file, max_bytes);
        Status ok = text ? Status{} : Status{text.error()};
        if (ok && validate) ok = validate(*text);
        if (ok) {
            out.bytes = std::move(text).value();
            out.generation = g;
            if (g > 0)
                VICTUS_LOG_WARN("fs", "'{}' recovered from generation {} ({})", path.string(), g,
                                out.rejected.empty() ? std::string("current file missing") : out.rejected.front());
            return out;
        }
        if (ok.error().code == ErrorCode::VersionMismatch)
            return make_error(ErrorCode::VersionMismatch, std::format("{}: {}", file.filename().string(), ok.error().message));
        out.rejected.push_back(std::format("{}: {}", file.filename().string(), ok.error().message));
    }
    if (!any) return make_error(ErrorCode::NotFound, std::format("'{}' does not exist", path.string()));
    std::string why;
    for (const auto& r : out.rejected) why += (why.empty() ? "" : "; ") + r;
    return make_error(ErrorCode::ParseError, std::format("no valid copy of '{}': {}", path.string(), why));
}

std::vector<std::uint32_t> generations(const Path& path, std::uint32_t history) {
    std::vector<std::uint32_t> out;
    std::error_code ec;
    for (std::uint32_t g = 1; g <= std::min(history, kMaxHistory); ++g)
        if (std::filesystem::is_regular_file(history_path(path, g), ec)) out.push_back(g);
    return out;
}

Status restore_generation(const Path& path, std::uint32_t generation, const SafeWriteOptions& options) {
    if (generation == 0 || generation > kMaxHistory)
        return make_error(ErrorCode::InvalidArgument, std::format("no generation {}", generation));
    auto text = read_text(history_path(path, generation));
    if (!text) return make_error(ErrorCode::NotFound, std::format("'{}' has no generation {}", path.filename().string(), generation));
    return write_safely(path, *text, options);
}

Result<std::string> sanitize_relative(std::string_view relative) {
    if (relative.empty()) return make_error(ErrorCode::InvalidArgument, "empty path");
    if (relative.find('\0') != std::string_view::npos) return make_error(ErrorCode::InvalidArgument, "path contains NUL");
    std::string s(relative);
    for (auto& c : s)
        if (c == '\\') c = '/';
    if (s.front() == '/' || (s.size() > 1 && s[1] == ':'))
        return make_error(ErrorCode::InvalidArgument, std::format("absolute path not allowed: '{}'", s));
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t end = s.find('/', start);
        const std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return make_error(ErrorCode::InvalidArgument, std::format("path escapes root: '{}'", s));
        if (!part.empty() && part != ".") parts.push_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (parts.empty()) return make_error(ErrorCode::InvalidArgument, "path resolves to root");
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

Result<Path> resolve_under(const Path& root, std::string_view relative) {
    auto clean = sanitize_relative(relative);
    if (!clean) return std::move(clean).error();
    return root / Path(*clean);
}

Path executable_path() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return Path(std::wstring(buf, n));
#elif defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return std::filesystem::weakly_canonical(buf);
    return {};
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? Path{} : p;
#endif
}

Path executable_dir() { return executable_path().parent_path(); }

Path user_data_dir() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) return Path(appdata) / "VICTUS";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) return Path(home) / "Library" / "Application Support" / "VICTUS";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return Path(xdg) / "victus";
    if (const char* home = std::getenv("HOME")) return Path(home) / ".config" / "victus";
#endif
    return std::filesystem::temp_directory_path() / "victus";
}

} // namespace victus::fs
