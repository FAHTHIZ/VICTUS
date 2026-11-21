// SPDX-License-Identifier: Apache-2.0
// U.S. National Park Service Sound Gallery (https://www.nps.gov/subjects/sound/gallery.htm):
// field recordings of wildlife, weather, water and human-made sounds from national parks.
// Works produced by NPS staff are U.S. federal government works and in the public domain
// (17 U.S.C. 105; https://www.nps.gov/aboutus/disclaimer.htm). Each sound page states its
// "Credit / Author"; VICTUS only installs a recording when that credit is the NPS itself, and
// treats any other credit as third-party content whose rights must be checked by hand.
#include "provider_util.hpp"

#include <format>
#include <mutex>

namespace victus::library {
namespace {

using namespace detail;

constexpr std::string_view kSite = "https://www.nps.gov";
constexpr std::string_view kGallery = "https://www.nps.gov/subjects/sound/gallery.htm";

class NpsSounds final : public Provider {
public:
    NpsSounds() {
        info_.id = "nps";
        info_.name = "National Park Service Sounds";
        info_.homepage = std::string(kGallery);
        info_.credit = "Sounds courtesy of the U.S. National Park Service";
        info_.description = "Public-domain field recordings from U.S. national parks: birds, mammals, insects, water and weather.";
        info_.types = {AssetType::Audio};
    }
    const ProviderInfo& info() const override { return info_; }

    Result<SearchPage> search(HttpClient& http, const SearchQuery& q) override {
        if (!wants(q, AssetType::Audio)) return SearchPage{};
        std::vector<LibraryAsset> all;
        {
            std::lock_guard lock(mutex_);
            if (catalogue_.empty()) {
                auto html = get_text(http, std::string(kGallery));
                if (!html) return html.error();
                catalogue_ = parse_gallery(html.value());
            }
            for (const auto& a : catalogue_)
                if (q.text.empty() || matches_words(q.text, {a.name, a.category})) all.push_back(a);
        }
        return paginate(std::move(all), q);
    }

    Result<InstallPlan> plan(HttpClient& http, const LibraryAsset& a, std::string_view /*variant*/) override {
        auto html = get_text(http, a.page_url);
        if (!html) return html.error();
        const std::string_view page = html.value();
        // The download link is set by script: element.setAttribute('href', 'https://...mp3')
        std::size_t p = 0;
        std::string mp3(between(page, "setAttribute('href', '", "'", p));
        if (mp3.empty() || !mp3.ends_with(".mp3")) {
            const std::size_t at = page.find(".mp3");
            if (at != std::string_view::npos) {
                const std::size_t start = page.rfind("https://", at);
                if (start != std::string_view::npos) mp3 = std::string(page.substr(start, at + 4 - start));
            }
        }
        if (mp3.empty() || mp3.find_first_of("\"'<> ") != std::string::npos)
            return make_error(ErrorCode::NotFound, "no recording found on " + a.page_url);
        const std::string credit = page_credit(page);
        LibraryAsset checked = a;
        checked.author = credit.empty() ? std::string("unknown") : credit;
        if (!credit_is_nps(credit)) {
            return make_error(ErrorCode::InvalidArgument, "'" + a.name + "' is credited to " + checked.author +
                                                              ", not the National Park Service, so it may be copyrighted. It was not installed.");
        }
        if (Status st = require_installable(checked); !st) return st.error();
        InstallPlan plan;
        FileRequest f;
        f.url = mp3;
        f.relative = slugify(a.name) + ".mp3";
        plan.files.push_back(std::move(f));
        plan.primary = plan.files.back().relative;
        return plan;
    }

    // Gallery layout: <h3><strong>Category</strong></h3> ... <a href="/subjects/sound/sounds-x.htm" ...>Name</a>
    static std::vector<LibraryAsset> parse_gallery(std::string_view html) {
        std::vector<LibraryAsset> out;
        std::string category;
        std::size_t at = 0;
        while (at < html.size()) {
            const std::size_t h3 = html.find("<h3>", at);
            const std::size_t link = html.find("<a href=\"/subjects/sound/sounds-", at);
            if (link == std::string_view::npos) break;
            if (h3 != std::string_view::npos && h3 < link) {
                std::size_t p = h3;
                category = strip_tags(between(html, "<h3>", "</h3>", p));
                at = p == std::string_view::npos ? html.size() : p;
                continue;
            }
            std::size_t p = link;
            const std::string_view href = between(html, "<a href=\"", "\"", p);
            if (p == std::string_view::npos) break;
            const std::size_t close = html.find('>', p);
            const std::size_t end = html.find("</a>", close);
            at = end == std::string_view::npos ? html.size() : end + 4;
            if (close == std::string_view::npos || end == std::string_view::npos) break;
            LibraryAsset a;
            a.provider = "nps";
            a.id = std::string(href.substr(href.rfind('/') + 1));
            if (a.id.ends_with(".htm")) a.id.resize(a.id.size() - 4);
            if (a.id.starts_with("sounds-")) a.id = a.id.substr(7);
            a.name = strip_tags(html.substr(close + 1, end - close - 1));
            a.category = category;
            a.type = AssetType::Audio;
            a.author = "U.S. National Park Service";
            a.page_url = std::string(kSite) + std::string(href);
            a.licence = licence_public_domain_us_gov();
            a.licence.notes = "U.S. government work. VICTUS checks the page's credit line on install and refuses recordings "
                              "credited to anyone other than the NPS.";
            a.variants = {{"mp3", "MP3", 0}};
            bool dup = false;
            for (const auto& o : out) dup = dup || o.id == a.id;
            if (!dup && !a.name.empty()) out.push_back(std::move(a));
        }
        return out;
    }

    static std::string page_credit(std::string_view page) {
        const std::size_t at = page.find("Credit / Author:");
        if (at == std::string_view::npos) return {};
        // <dt>Credit / Author:</dt>\n<dd>NPS</dd>
        std::size_t p = at;
        const std::string_view dd = between(page, "<dd>", "</dd>", p);
        if (p == std::string_view::npos || p - at > 400) return {};
        return strip_tags(dd);
    }

    static bool credit_is_nps(std::string_view credit) {
        const std::string c = lower(credit);
        return c == "nps" || c.starts_with("nps ") || c.starts_with("nps/") || c.starts_with("nps,") ||
               c.starts_with("national park service");
    }

private:
    ProviderInfo info_;
    std::mutex mutex_;
    std::vector<LibraryAsset> catalogue_;
};

} // namespace

std::unique_ptr<Provider> make_nps_sounds() { return std::make_unique<NpsSounds>(); }

} // namespace victus::library
