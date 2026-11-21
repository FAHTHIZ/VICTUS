// SPDX-License-Identifier: Apache-2.0
// BigSoundBank (https://bigsoundbank.com) by Joseph Sardin: thousands of free sound effects and
// ambiences. Licence (https://bigsoundbank.com/droit.html, checked 2026-11-21): the sounds are
// royalty free under CC0 / WTFPL, including commercial use and modification; credit
// ("Joseph SARDIN - BigSoundBank.com") is appreciated but optional.
//
// The site has no JSON API, so search reads its public search page. Requests are only made when
// the user searches or installs (never in bulk), matching the site's ai.txt allowance for
// real-time user-initiated fetching.
#include "provider_util.hpp"

#include <format>

namespace victus::library {
namespace {

using namespace detail;

constexpr std::string_view kSite = "https://bigsoundbank.com";

class BigSoundBank final : public Provider {
public:
    BigSoundBank() {
        info_.id = "bigsoundbank";
        info_.name = "BigSoundBank";
        info_.homepage = "https://bigsoundbank.com";
        info_.credit = "Sounds by Joseph SARDIN - BigSoundBank.com";
        info_.description = "Free CC0 sound effects and ambiences: doors, vehicles, nature, crowds, impacts and more.";
        info_.types = {AssetType::Audio};
    }
    const ProviderInfo& info() const override { return info_; }

    Result<SearchPage> search(HttpClient& http, const SearchQuery& q) override {
        if (!wants(q, AssetType::Audio)) return SearchPage{};
        const std::string words = q.text.empty() ? std::string("ambience") : q.text;
        auto html = get_text(http, std::format("{}/search?q={}", kSite, url_encode(words)));
        if (!html) return html.error();
        return paginate(parse(html.value()), q);
    }

    Result<InstallPlan> plan(HttpClient& /*http*/, const LibraryAsset& a, std::string_view /*variant*/) override {
        if (Status st = require_installable(a); !st) return st.error();
        if (a.id.empty() || a.id.find_first_not_of("0123456789") != std::string::npos)
            return make_error(ErrorCode::InvalidArgument, "invalid BigSoundBank sound id");
        InstallPlan plan;
        FileRequest f;
        f.url = std::format("{}/UPLOAD/ogg/{}.ogg", kSite, a.id);
        f.relative = std::format("{}_{}.ogg", slugify(a.name), a.id);
        plan.files.push_back(std::move(f));
        plan.primary = plan.files.back().relative;
        return plan;
    }

    // One result block per sound: <h2><a href='/name-s0015.html'>Name</a>...<audio ...
    // src="/UPLOAD/mp3/0015.mp3">...<div>Description. UCS Category: CAT. Length: 00:05.</div>
    static std::vector<LibraryAsset> parse(std::string_view html) {
        std::vector<LibraryAsset> out;
        std::size_t at = 0;
        while (true) {
            const std::size_t block = html.find("<div class='resultat_", at);
            if (block == std::string_view::npos) break;
            const std::size_t next = html.find("<div class='resultat_", block + 10);
            const std::string_view b = html.substr(block, (next == std::string_view::npos ? html.size() : next) - block);
            at = block + 10;
            std::size_t p = 0;
            const std::string_view href = between(b, "<a href='/", "'>", p);
            if (p == std::string_view::npos) continue;
            const std::string_view title = between(b, "", "</a>", p);
            std::size_t ap = 0;
            const std::string_view id = between(b, "/UPLOAD/mp3/", ".mp3", ap);
            if (id.empty() || id.find_first_not_of("0123456789") != std::string_view::npos) continue;
            LibraryAsset a;
            a.provider = "bigsoundbank";
            a.id = std::string(id);
            a.name = strip_tags(title);
            a.type = AssetType::Audio;
            a.author = "Joseph SARDIN";
            a.page_url = std::format("{}/{}", kSite, href);
            std::size_t dp = b.find("</svg></button></div></div><div>");
            if (dp != std::string_view::npos) {
                dp += std::string_view("</svg></button></div></div><div>").size();
                const std::size_t end = b.find("</div>", dp);
                std::string desc = strip_tags(b.substr(dp, end - dp));
                if (const std::size_t ucs = desc.find("UCS Category:"); ucs != std::string::npos) {
                    const std::size_t dot = desc.find('.', ucs);
                    a.category = strip_tags(desc.substr(ucs + 13, dot == std::string::npos ? std::string::npos : dot - ucs - 13));
                    std::string rest = dot == std::string::npos ? std::string{} : desc.substr(dot + 1);
                    desc = desc.substr(0, ucs) + rest;
                }
                a.description = strip_tags(desc);
            }
            a.thumbnail_url.clear(); // sounds have no image; Studio draws a waveform icon
            a.licence = licence_cc0();
            a.licence.name = "CC0 1.0 / WTFPL (BigSoundBank)";
            a.licence.notes = "Free for any use including commercial; credit \"Joseph SARDIN - BigSoundBank.com\" is appreciated.";
            a.variants = {{"ogg", "Ogg Vorbis", 0}};
            json::Value extra = json::Value::object();
            extra.set("preview", std::format("{}/UPLOAD/ogg/{}.ogg", kSite, a.id));
            a.extra = std::move(extra);
            bool dup = false;
            for (const auto& o : out) dup = dup || o.id == a.id;
            if (!dup) out.push_back(std::move(a));
        }
        return out;
    }

private:
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<Provider> make_bigsoundbank() { return std::make_unique<BigSoundBank>(); }

} // namespace victus::library
