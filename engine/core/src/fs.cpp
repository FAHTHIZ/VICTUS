// SPDX-License-Identifier: Apache-2.0
#include "victus/core/fs.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <random>

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

Result<std::vector<std::uint8_t>> read_binary(const Path& path, std::uint64_t max_bytes) {
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
