// SPDX-License-Identifier: Apache-2.0
// Sketchfab (https://sketchfab.com): downloadable models under CC0 and CC BY through the Data API
// v3 (https://sketchfab.com/developers/data-api/v3). Searching is public; downloading needs the
// user's own Sketchfab account (API token from https://sketchfab.com/settings/password), as the
// Download API requires. Only CC0 and CC BY results are listed: NC/ND licences forbid use in a
// commercial or modified game, and CC BY-SA would impose share-alike on the user's adaptations,
// so neither is offered. CC BY attribution (title, author, link, licence) is written to the
// project's CREDITS.md on install.
#include "provider_util.hpp"

#include <format>

namespace victus::library {
namespace {

using namespace detail;

constexpr std::string_view kApi = "https://api.sketchfab.com/v3";

LibraryAsset to_asset(const json::Value& r) {
    LibraryAsset a;
    a.provider = "sketchfab";
    a.id = r["uid"].as_string_or("");
    a.name = r["name"].as_string_or("Untitled");
    a.type = AssetType::Model;
    a.author = r["user"]["displayName"].as_string_or(r["user"]["username"].as_string_or(""));
    a.description = r["description"].as_string_or("");
    if (a.description.size() > 600) a.description = a.description.substr(0, 600) + "...";
    for (const auto& t : r["tags"].as_array()) {
        const std::string tag = t["name"].as_string_or("");
        if (!tag.empty()) a.tags.push_back(tag);
    }
    for (const auto& c : r["categories"].as_array())
        if (a.category.empty()) a.category = c["name"].as_string_or("");
    a.page_url = "https://sketchfab.com/3d-models/" + a.id;
    int best = 0;
    for (const auto& img : r["thumbnails"]["images"].as_array()) {
        const int w = static_cast<int>(img["width"].as_int(0));
        // The smallest image at least 200 px wide (else the largest).
        if (a.thumbnail_url.empty() || (w >= 200 && (best < 200 || w < best)) || (best < 200 && w > best)) {
            a.thumbnail_url = img["url"].as_string_or("");
            best = w;
        }
    }
    a.licence = licence_from_cc_label(r["license"]["label"].as_string_or(r["license"]["slug"].as_string_or("")));
    a.requires_account = true;
    const json::Value& glb = r["archives"]["glb"];
    const auto bytes = static_cast<std::uint64_t>(glb["size"].as_int(0));
    a.variants = {{"glb", bytes ? std::format("GLB ({})", format_bytes(bytes)) : std::string("GLB"), bytes}};
    json::Value extra = json::Value::object();
    extra.set("faces", r["faceCount"].as_int(0));
    extra.set("viewer", r["viewerUrl"].as_string_or(a.page_url));
    a.extra = std::move(extra);
    return a;
}

class Sketchfab final : public Provider {
public:
    Sketchfab() {
        info_.id = "sketchfab";
        info_.name = "Sketchfab";
        info_.homepage = "https://sketchfab.com";
        info_.credit = "Models from Sketchfab, credited to their authors";
        info_.description = "Hundreds of thousands of downloadable CC0 and CC BY models by independent artists and museums.";
        info_.types = {AssetType::Model};
        info_.needs_api_key = true;
        info_.api_key_help = "Downloads use your own Sketchfab account. Paste the API token from "
                             "https://sketchfab.com/settings/password (it is stored only in your user settings).";
    }
    const ProviderInfo& info() const override { return info_; }
    void set_api_key(std::string key) override { token_ = std::move(key); }

    Result<SearchPage> search(HttpClient& http, const SearchQuery& q) override {
        if (!wants(q, AssetType::Model)) return SearchPage{};
        // One request per accepted licence (the API honours a single `license` filter), merged.
        const std::uint32_t half = std::max<std::uint32_t>(1, (q.page_size + 1) / 2);
        SearchPage page;
        std::vector<std::vector<LibraryAsset>> lists;
        for (const char* licence : {"cc0", "by"}) {
            std::string url = std::format("{}/search?type=models&downloadable=true&license={}&count={}&cursor={}",
                                          kApi, licence, half, static_cast<std::uint64_t>(q.page) * half);
            if (!q.text.empty()) url += "&q=" + url_encode(q.text);
            auto v = get_json(http, url);
            if (!v) return v.error();
            std::vector<LibraryAsset> list;
            for (const auto& r : v.value()["results"].as_array()) {
                if (r["isAgeRestricted"].as_bool(false)) continue;
                LibraryAsset a = to_asset(r);
                if (!a.id.empty() && a.licence.installable() && !a.licence.share_alike) list.push_back(std::move(a));
            }
            if (v.value()["next"].is_string()) page.has_more = true;
            lists.push_back(std::move(list));
        }
        for (std::size_t i = 0; i < std::max(lists[0].size(), lists[1].size()); ++i)
            for (auto& list : lists)
                if (i < list.size()) page.assets.push_back(std::move(list[i]));
        page.total = page.assets.size() + (page.has_more ? 1 : 0);
        return page;
    }

    Result<InstallPlan> plan(HttpClient& http, const LibraryAsset& a, std::string_view /*variant*/) override {
        if (Status st = require_installable(a); !st) return st.error();
        if (token_.empty())
            return make_error(ErrorCode::InvalidArgument, "Sketchfab downloads need your Sketchfab API token. " + info_.api_key_help);
        const std::vector<HttpHeader> auth{{"Authorization", "Token " + token_}};
        auto v = get_json(http, std::format("{}/models/{}/download", kApi, url_encode(a.id)), auth);
        if (!v) return v.error();
        const json::Value& glb = v.value()["glb"];
        const std::string url = glb["url"].as_string_or("");
        if (url.empty()) return make_error(ErrorCode::NotFound, "Sketchfab did not offer a GLB download for '" + a.name + "'");
        InstallPlan plan;
        FileRequest f;
        f.url = url; // short-lived signed URL; no credentials are sent to it
        f.bytes = static_cast<std::uint64_t>(glb["size"].as_int(0));
        f.relative = slugify(a.name) + ".glb";
        plan.files.push_back(std::move(f));
        plan.primary = plan.files.back().relative;
        return plan;
    }

private:
    ProviderInfo info_;
    std::string token_;
};

} // namespace

std::unique_ptr<Provider> make_sketchfab() { return std::make_unique<Sketchfab>(); }

} // namespace victus::library
