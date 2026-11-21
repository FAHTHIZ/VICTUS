// SPDX-License-Identifier: Apache-2.0
// Offline fixtures for the Asset Store tests: an HttpClient that serves canned responses by URL
// (no network in CI) and a temporary project folder.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/library/http.hpp"

#include "stb_image_write.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace victus::library::test {

class FixtureHttp final : public HttpClient {
public:
    std::map<std::string, std::string> responses; // url -> body (prefix match when key ends with '*')
    std::vector<std::string> requested;
    std::vector<std::vector<HttpHeader>> headers;
    bool fail_downloads{false};

    const std::string* find(const std::string& url) const {
        if (auto it = responses.find(url); it != responses.end()) return &it->second;
        for (const auto& [k, v] : responses)
            if (!k.empty() && k.back() == '*' && url.starts_with(k.substr(0, k.size() - 1))) return &v;
        return nullptr;
    }
    Result<std::string> get(const HttpRequest& r, const std::atomic<bool>* cancel) override {
        requested.push_back(r.url);
        headers.push_back(r.headers);
        if (cancel && cancel->load()) return make_error(ErrorCode::Cancelled, "cancelled");
        const std::string* body = find(r.url);
        if (!body) return make_error(ErrorCode::NetworkError, "HTTP 404 " + r.url);
        if (body->size() > r.max_bytes) return make_error(ErrorCode::LimitExceeded, "too large");
        return *body;
    }
    Status download(const HttpRequest& r, const fs::Path& dest, const Progress& progress, const std::atomic<bool>* cancel) override {
        requested.push_back(r.url);
        headers.push_back(r.headers);
        if (cancel && cancel->load()) return make_error(ErrorCode::Cancelled, "cancelled");
        if (fail_downloads) return make_error(ErrorCode::NetworkError, "connection reset");
        const std::string* body = find(r.url);
        if (!body) return make_error(ErrorCode::NetworkError, "HTTP 404 " + r.url);
        if (body->size() > r.max_bytes) return make_error(ErrorCode::LimitExceeded, "too large");
        if (progress) progress(body->size(), body->size());
        return fs::write_text_atomic(dest, *body);
    }
};

struct TempProject {
    fs::Path root;
    explicit TempProject(const char* name) {
        root = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(root / "assets");
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    TempProject(const TempProject&) = delete;
    TempProject& operator=(const TempProject&) = delete;
};

// A w x h single-colour PNG (RGB or grey) as a string body.
inline std::string png(int w, int h, int channels, std::uint8_t value) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h * channels), value);
    std::string out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<std::size_t>(size)); },
        &out, w, h, channels, px.data(), w * channels);
    return out;
}

} // namespace victus::library::test
