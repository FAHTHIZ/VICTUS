// SPDX-License-Identifier: Apache-2.0
// Kenney (https://kenney.nl): CC0 game asset packs (3D kits, characters, sprites, UI, audio).
// Licence (https://kenney.nl/support, checked 2026-11-21): CC0, free for personal and commercial
// projects, credit appreciated but not required.
//
// Kenney's downloads are served through short-lived links behind the site's optional donation
// dialog, so VICTUS does not download packs itself. The store browses the public catalogue
// (https://kenney.nl/assets?search=...), opens the pack page in the user's browser, and imports
// the ZIP the user downloaded with import_archive(), recording the same licence provenance.
#include "provider_util.hpp"

#include <format>

namespace victus::library {
namespace {

using namespace detail;

constexpr std::string_view kSite = "https://kenney.nl";

AssetType type_for(std::string_view category) {
    if (category == "3D") return AssetType::Model;
    if (category == "Audio") return AssetType::Audio;
    if (category == "2D" || category == "Textures") return AssetType::Texture;
    return AssetType::Pack;
}

class Kenney final : public Provider {
public:
    Kenney() {
        info_.id = "kenney";
        info_.name = "Kenney";
        info_.homepage = "https://kenney.nl/assets";
        info_.credit = "Game assets by Kenney (www.kenney.nl), CC0";
        info_.description = "Hundreds of consistent CC0 kits: low-poly 3D, characters, sprites, UI, fonts and sound effects.";
        info_.types = {AssetType::Model, AssetType::Texture, AssetType::Audio, AssetType::Pack};
        info_.browse_only = true;
    }
    const ProviderInfo& info() const override { return info_; }

    Result<SearchPage> search(HttpClient& http, const SearchQuery& q) override {
        std::string base = std::string(kSite) + "/assets";
        if (q.type == static_cast<int>(AssetType::Model)) base += "/category:3D";
        else if (q.type == static_cast<int>(AssetType::Audio)) base += "/category:Audio";
        else if (q.type == static_cast<int>(AssetType::Texture)) base += "/category:2D";
        else if (q.type >= 0 && q.type != static_cast<int>(AssetType::Pack)) return SearchPage{};
        const std::uint32_t page_no = q.page + 1;
        std::string url = page_no > 1 ? std::format("{}/page:{}", base, page_no) : base;
        if (!q.text.empty()) url += "?search=" + url_encode(q.text);
        auto html = get_text(http, url);
        if (!html) return html.error();
        SearchPage page;
        page.assets = parse(html.value());
        page.has_more = html.value().find(std::format("page:{}", page_no + 1)) != std::string::npos;
        page.total = static_cast<std::uint64_t>(q.page) * 16 + page.assets.size() + (page.has_more ? 16 : 0);
        return page;
    }

    Result<InstallPlan> plan(HttpClient& /*http*/, const LibraryAsset& a, std::string_view /*variant*/) override {
        return make_error(ErrorCode::Unsupported, "Download '" + a.name + "' from " + a.page_url +
                                                      ", then choose \"Import downloaded ZIP\" to add it to the project.");
    }

    // <div class='asset'><a href='https://kenney.nl/assets/slug'><div class='cover'
    //  style='background-image:url("https://.../sample-400x.png")'></div></a>
    //  <h2><a href='...'>Name</a></h2><span ...><a href='.../category:3D'>3D</a> ... series:Name
    static std::vector<LibraryAsset> parse(std::string_view html) {
        std::vector<LibraryAsset> out;
        std::size_t at = 0;
        while (true) {
            const std::size_t block = html.find("<div class='asset'>", at);
            if (block == std::string_view::npos) break;
            const std::size_t next = html.find("<div class='asset'>", block + 10);
            const std::string_view b = html.substr(block, (next == std::string_view::npos ? html.size() : next) - block);
            at = block + 10;
            std::size_t p = 0;
            const std::string_view href = between(b, "<a href='", "'", p);
            const std::string prefix = std::string(kSite) + "/assets/";
            if (!href.starts_with(prefix)) continue;
            LibraryAsset a;
            a.provider = "kenney";
            a.id = std::string(href.substr(prefix.size()));
            if (a.id.empty() || a.id.find_first_of(":/?") != std::string::npos) continue;
            p = 0;
            a.thumbnail_url = std::string(between(b, "url(\"", "\")", p));
            if (!a.thumbnail_url.starts_with("https://")) a.thumbnail_url.clear();
            p = 0;
            const std::string_view h2 = between(b, "<h2>", "</h2>", p);
            a.name = strip_tags(h2);
            p = 0;
            a.category = std::string(between(b, "/category:", "'", p));
            p = 0;
            const std::string_view series = between(b, "/series:", "'", p);
            if (!series.empty()) a.tags.push_back(html_unescape(std::string(series)));
            a.type = type_for(a.category);
            a.author = "Kenney";
            a.page_url = std::string(href);
            a.licence = licence_cc0();
            a.licence.notes = "Kenney assets are CC0. Credit \"Kenney (www.kenney.nl)\" is appreciated, not required.";
            a.description = "Download the pack from Kenney's site (an optional donation supports new packs), then import the ZIP.";
            out.push_back(std::move(a));
        }
        return out;
    }

private:
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<Provider> make_kenney() { return std::make_unique<Kenney>(); }

} // namespace victus::library
