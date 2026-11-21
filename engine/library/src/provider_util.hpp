// SPDX-License-Identifier: Apache-2.0
// Shared helpers for the built-in providers (private to the library module).
#pragma once
#include "victus/library/library.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace victus::library::detail {

inline Result<json::Value> get_json(HttpClient& http, std::string url, std::vector<HttpHeader> headers = {},
                                    std::uint64_t max_bytes = 32ull << 20) {
    HttpRequest req;
    req.url = std::move(url);
    req.headers = std::move(headers);
    req.max_bytes = max_bytes;
    req.timeout_seconds = 45;
    auto body = http.get(req);
    if (!body) return body.error();
    auto parsed = json::parse(body.value());
    if (!parsed) return make_error(ErrorCode::ParseError, "unexpected response from " + req.url + ": " + parsed.error().message);
    return std::move(parsed.value());
}

inline Result<std::string> get_text(HttpClient& http, std::string url, std::uint64_t max_bytes = 8ull << 20) {
    HttpRequest req;
    req.url = std::move(url);
    req.max_bytes = max_bytes;
    req.timeout_seconds = 45;
    return http.get(req);
}

inline std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Every whitespace-separated word of `query` occurs in one of `fields` (ASCII case-insensitive).
inline bool matches_words(std::string_view query, const std::vector<std::string_view>& fields) {
    std::string q = lower(query);
    std::size_t at = 0;
    while (at < q.size()) {
        while (at < q.size() && q[at] == ' ') ++at;
        const std::size_t end = std::min(q.find(' ', at), q.size());
        if (end > at) {
            const std::string_view word(q.data() + at, end - at);
            bool found = false;
            for (const std::string_view f : fields)
                if (lower(f).find(word) != std::string::npos) {
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        at = end;
    }
    return true;
}

inline SearchPage paginate(std::vector<LibraryAsset> all, const SearchQuery& q) {
    SearchPage page;
    page.total = all.size();
    const std::size_t size = std::max<std::uint32_t>(1, q.page_size);
    const std::size_t begin = std::min<std::size_t>(static_cast<std::size_t>(q.page) * size, all.size());
    const std::size_t end = std::min(begin + size, all.size());
    page.assets.assign(std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(begin)),
                       std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(end)));
    page.has_more = end < all.size();
    return page;
}

inline bool wants(const SearchQuery& q, AssetType t) { return q.type < 0 || q.type == static_cast<int>(t); }

inline Status require_installable(const LibraryAsset& a) {
    if (a.licence.installable()) return {};
    return make_error(ErrorCode::InvalidArgument,
                      "'" + a.name + "' is licensed under " + a.licence.name +
                          ", which does not allow use in a game that may be sold or modified. It was not installed.");
}

// Text between `open` and `close` starting at `from`; advances `from` past `close`.
inline std::string_view between(std::string_view s, std::string_view open, std::string_view close, std::size_t& from) {
    const std::size_t a = s.find(open, from);
    if (a == std::string_view::npos) {
        from = std::string_view::npos;
        return {};
    }
    const std::size_t b = s.find(close, a + open.size());
    if (b == std::string_view::npos) {
        from = std::string_view::npos;
        return {};
    }
    from = b + close.size();
    return s.substr(a + open.size(), b - a - open.size());
}

inline std::string strip_tags(std::string_view html) {
    std::string out;
    bool in_tag = false;
    for (const char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out.push_back(c);
    }
    std::string trimmed = html_unescape(out);
    const auto first = trimmed.find_first_not_of(" \t\r\n");
    const auto last = trimmed.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : trimmed.substr(first, last - first + 1);
}

// File extension of a URL path in lower case, without query ("jpg").
inline std::string url_extension(std::string_view url) {
    const std::size_t q = std::min(url.find_first_of("?#"), url.size());
    const std::string_view path = url.substr(0, q);
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return lower(path.substr(dot + 1));
}

} // namespace victus::library::detail
