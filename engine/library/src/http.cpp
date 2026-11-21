// SPDX-License-Identifier: Apache-2.0
#include "victus/library/http.hpp"

#include "victus/core/hash.hpp"
#include "victus/core/process.hpp"
#include "victus/library/library.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <random>
#include <system_error>
#include <thread>

namespace victus::library {

bool is_safe_https_url(std::string_view url) noexcept {
    constexpr std::string_view scheme = "https://";
    if (url.size() <= scheme.size() || url.size() > 4096 || url.substr(0, scheme.size()) != scheme) return false;
    const std::string_view rest = url.substr(scheme.size());
    const std::size_t host_end = std::min(rest.find_first_of("/?#"), rest.size());
    const std::string_view host = rest.substr(0, host_end);
    if (host.empty() || host.find('@') != std::string_view::npos) return false;
    for (const char c : host)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':'))
            return false;
    for (const char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '\\' || static_cast<unsigned char>(c) == 0x7f) return false;
    return true;
}

namespace {

fs::Path temp_file(std::string_view tag) {
    std::random_device rd;
    std::error_code ec;
    fs::Path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = fs::user_data_dir();
    return dir / std::format("victus-http-{}-{:08x}{:08x}", tag, rd(), rd());
}

void remove_quietly(const fs::Path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

Status check_header(const HttpHeader& h) {
    const auto bad = [](char c) { return c == '\r' || c == '\n' || c == '\0'; };
    for (const char c : h.name)
        if (bad(c) || c == ':') return make_error(ErrorCode::InvalidArgument, "http: invalid header name");
    for (const char c : h.value)
        if (bad(c)) return make_error(ErrorCode::InvalidArgument, "http: invalid header value");
    return {};
}

std::string curl_error(int code, const fs::Path& err_file) {
    auto text = fs::read_text(err_file, 64 * 1024);
    std::string detail = text ? text.value() : std::string{};
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
    if (const auto at = detail.rfind('\n'); at != std::string::npos) detail = detail.substr(at + 1);
    std::string what;
    switch (code) {
    case 6: what = "could not resolve the host (offline?)"; break;
    case 7: what = "could not connect"; break;
    case 22: what = "the server returned an error"; break;
    case 28: what = "timed out"; break;
    case 35:
    case 60: what = "secure connection failed"; break;
    case 47: what = "too many redirects"; break;
    case 63: what = "the file is larger than allowed"; break;
    default: what = std::format("curl exit code {}", code); break;
    }
    return detail.empty() ? what : what + " (" + detail + ")";
}

class CurlClient final : public HttpClient {
public:
    Result<std::string> get(const HttpRequest& request, const std::atomic<bool>* cancel) override {
        const fs::Path tmp = temp_file("get");
        Status st = download(request, tmp, {}, cancel);
        if (!st) return st.error();
        auto text = fs::read_text(tmp, request.max_bytes + 1);
        remove_quietly(tmp);
        if (!text) return text.error();
        return std::move(text.value());
    }

    Status download(const HttpRequest& request, const fs::Path& destination, const Progress& progress,
                    const std::atomic<bool>* cancel) override {
        if (!is_safe_https_url(request.url)) return make_error(ErrorCode::InvalidArgument, "http: only https:// URLs are allowed");
        auto curl = process::find_executable("curl");
        if (!curl) return make_error(ErrorCode::Unsupported, "http: the 'curl' program was not found on PATH (needed for Asset Store downloads)");
        const fs::Path part = destination.string() + ".part";
        const fs::Path err = temp_file("err");
        std::error_code ec;
        if (destination.has_parent_path()) std::filesystem::create_directories(destination.parent_path(), ec);
        process::LaunchOptions lo;
        lo.args = {curl.value(), "--silent", "--show-error", "--location", "--fail", "--proto", "=https", "--proto-redir", "=https",
                   "--max-redirs", "8", "--connect-timeout", "20", "--max-time", std::to_string(std::max(5, request.timeout_seconds)),
                   "--max-filesize", std::to_string(request.max_bytes), "--user-agent", user_agent(), "--stderr", err.string(),
                   "--output", part.string()};
        for (const HttpHeader& h : request.headers) {
            if (Status hs = check_header(h); !hs) return hs;
            lo.args.push_back("--header");
            lo.args.push_back(h.name + ": " + h.value);
        }
        lo.args.push_back("--url");
        lo.args.push_back(request.url);
        lo.discard_stderr = true;
        auto child = process::ChildProcess::spawn(lo);
        if (!child) return child.error();
        child.value()->close_stdin();
        std::optional<int> code;
        while (!(code = child.value()->wait(100))) {
            if (cancel && cancel->load()) {
                child.value()->kill();
                (void)child.value()->wait(2000);
                remove_quietly(part);
                remove_quietly(err);
                return make_error(ErrorCode::Cancelled, "http: cancelled");
            }
            if (progress) {
                const auto size = std::filesystem::file_size(part, ec);
                progress(ec ? 0 : size, request.expected_bytes);
            }
        }
        if (*code != 0) {
            std::string msg = curl_error(*code, err);
            remove_quietly(part);
            remove_quietly(err);
            return make_error(*code == 63 ? ErrorCode::LimitExceeded : ErrorCode::NetworkError, "http: " + request.url + ": " + msg);
        }
        remove_quietly(err);
        const auto size = std::filesystem::file_size(part, ec);
        if (ec) return make_error(ErrorCode::IoError, "http: no data received from " + request.url);
        if (size > request.max_bytes) {
            remove_quietly(part);
            return make_error(ErrorCode::LimitExceeded, "http: response exceeds the size limit");
        }
        if (progress) progress(size, request.expected_bytes ? request.expected_bytes : size);
        std::filesystem::rename(part, destination, ec);
        if (ec) {
            remove_quietly(part);
            return make_error(ErrorCode::IoError, "http: could not move the download into place: " + ec.message());
        }
        return {};
    }
};

} // namespace

std::unique_ptr<HttpClient> make_curl_client() { return std::make_unique<CurlClient>(); }

CachingHttpClient::CachingHttpClient(HttpClient& inner, fs::Path directory, std::chrono::seconds max_age)
    : inner_(inner), dir_(std::move(directory)), max_age_(max_age) {}

fs::Path CachingHttpClient::entry_path(std::string_view url) const { return dir_ / std::format("{:016x}.cache", fnv1a64(url)); }

Result<std::string> CachingHttpClient::get(const HttpRequest& request, const std::atomic<bool>* cancel) {
    // Requests with credentials are never cached (they may differ per account).
    const bool cacheable = request.headers.empty();
    const fs::Path entry = entry_path(request.url);
    std::error_code ec;
    if (cacheable) {
        std::lock_guard lock(mutex_);
        const auto stamp = std::filesystem::last_write_time(entry, ec);
        if (!ec) {
            const auto age = std::filesystem::file_time_type::clock::now() - stamp;
            if (offline_ || age < max_age_) {
                auto text = fs::read_text(entry, request.max_bytes + 1);
                // The first line is the URL, so a hash collision never serves another page.
                if (text && text->starts_with(request.url + "\n")) return text->substr(request.url.size() + 1);
            }
        }
    }
    if (offline_) return make_error(ErrorCode::NetworkError, "offline: no cached copy of " + request.url);
    auto body = inner_.get(request, cancel);
    if (!body) {
        // Serve a stale copy rather than nothing when the network fails.
        if (cacheable && body.error().code == ErrorCode::NetworkError) {
            std::lock_guard lock(mutex_);
            auto text = fs::read_text(entry, request.max_bytes + 1);
            if (text && text->starts_with(request.url + "\n")) return text->substr(request.url.size() + 1);
        }
        return body;
    }
    if (cacheable) {
        std::lock_guard lock(mutex_);
        std::filesystem::create_directories(dir_, ec);
        (void)fs::write_text_atomic(entry, request.url + "\n" + body.value());
    }
    return body;
}

Status CachingHttpClient::download(const HttpRequest& request, const fs::Path& destination, const Progress& progress,
                                   const std::atomic<bool>* cancel) {
    if (offline_) return make_error(ErrorCode::NetworkError, "offline: downloads are disabled");
    return inner_.download(request, destination, progress, cancel);
}

} // namespace victus::library
