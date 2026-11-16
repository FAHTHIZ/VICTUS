// SPDX-License-Identifier: Apache-2.0
#include "victus/core/fs.hpp"

#include "victus/core/pack.hpp"

#include <cstdio>
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

Status write_atomic(const Path& path, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::random_device rd;
    Path tmp = path;
    tmp += std::format(".tmp{:08x}", rd());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return make_error(ErrorCode::IoError, std::format("cannot create '{}'", tmp.string()));
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return make_error(ErrorCode::IoError, std::format("write failed for '{}'", tmp.string()));
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return make_error(ErrorCode::IoError, std::format("cannot replace '{}'", path.string()));
    }
    return {};
}

Status write_text_atomic(const Path& path, std::string_view text) {
    return write_atomic(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
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
