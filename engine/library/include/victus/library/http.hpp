// SPDX-License-Identifier: Apache-2.0
// HTTPS transport for the Asset Store behind a VICTUS-owned interface (ADR-0174).
//
// The production client runs the system `curl` executable (shipped with Windows 10 1803+,
// macOS and every mainstream Linux distribution) through victus::process, so VICTUS links no
// TLS stack and inherits the platform's certificate store and proxy settings. Only https://
// URLs are allowed, also across redirects, and every transfer has a size cap and a timeout.
// Tests use a client that maps URLs to local files (tests/library).
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace victus::library {

struct HttpHeader {
    std::string name, value;
};

struct HttpRequest {
    std::string url;
    std::vector<HttpHeader> headers;
    std::uint64_t max_bytes{64ull << 20}; // responses larger than this fail (LimitExceeded)
    std::uint64_t expected_bytes{0};      // for progress when the server sends no length
    int timeout_seconds{60};              // whole transfer
};

// (bytes received, bytes expected or 0 when unknown)
using Progress = std::function<void(std::uint64_t, std::uint64_t)>;

class HttpClient {
public:
    virtual ~HttpClient() = default;
    // Body of a GET as text. Fails with NetworkError (unreachable, HTTP >= 400), LimitExceeded
    // or Cancelled when `cancel` becomes true.
    [[nodiscard]] virtual Result<std::string> get(const HttpRequest& request, const std::atomic<bool>* cancel = nullptr) = 0;
    // Streams a GET into `destination` (written as a whole: a failed transfer leaves no file).
    [[nodiscard]] virtual Status download(const HttpRequest& request, const fs::Path& destination,
                                          const Progress& progress = {}, const std::atomic<bool>* cancel = nullptr) = 0;
};

// The curl-backed client. Fails at the first request (Unsupported) when curl is missing.
[[nodiscard]] std::unique_ptr<HttpClient> make_curl_client();

// Caches successful get() responses on disk for `max_age`, keyed by URL (search pages, API
// indexes), so browsing does not refetch the same listing. download() is passed through.
class CachingHttpClient final : public HttpClient {
public:
    CachingHttpClient(HttpClient& inner, fs::Path directory, std::chrono::seconds max_age);
    [[nodiscard]] Result<std::string> get(const HttpRequest& request, const std::atomic<bool>* cancel = nullptr) override;
    [[nodiscard]] Status download(const HttpRequest& request, const fs::Path& destination, const Progress& progress = {},
                                  const std::atomic<bool>* cancel = nullptr) override;
    void set_offline(bool offline) noexcept { offline_ = offline; } // serve only cached responses
    [[nodiscard]] fs::Path entry_path(std::string_view url) const;

private:
    HttpClient& inner_;
    fs::Path dir_;
    std::chrono::seconds max_age_;
    std::atomic<bool> offline_{false};
    std::mutex mutex_;
};

// True for "https://host/..." with a plausible host (no credentials, spaces or control bytes).
[[nodiscard]] bool is_safe_https_url(std::string_view url) noexcept;

} // namespace victus::library
