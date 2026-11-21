// SPDX-License-Identifier: Apache-2.0
#include "victus/library/library.hpp"

#include "victus/core/build_info.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace victus::library {

namespace {
constexpr std::array<const char*, static_cast<std::size_t>(AssetType::Count)> kTypeNames = {
    "model", "material", "hdri", "texture", "audio", "pack"};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

const char* to_string(AssetType type) noexcept {
    const auto i = static_cast<std::size_t>(type);
    return i < kTypeNames.size() ? kTypeNames[i] : "model";
}

AssetType asset_type_from_string(std::string_view s) noexcept {
    for (std::size_t i = 0; i < kTypeNames.size(); ++i)
        if (s == kTypeNames[i]) return static_cast<AssetType>(i);
    return AssetType::Model;
}

json::Value Licence::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id);
    v.set("name", name);
    v.set("url", url);
    v.set("attribution_required", attribution_required);
    v.set("commercial_use", commercial_use);
    v.set("modification", modification);
    v.set("share_alike", share_alike);
    v.set("redistribute_in_game", redistribute_in_game);
    if (!notes.empty()) v.set("notes", notes);
    return v;
}

Licence Licence::from_json(const json::Value& v) {
    Licence l;
    l.id = v["id"].as_string_or("");
    l.name = v["name"].as_string_or(l.id);
    l.url = v["url"].as_string_or("");
    // Missing permission flags are treated as "not allowed": a record must state them.
    l.attribution_required = v["attribution_required"].as_bool(true);
    l.commercial_use = v["commercial_use"].as_bool(false);
    l.modification = v["modification"].as_bool(false);
    l.share_alike = v["share_alike"].as_bool(false);
    l.redistribute_in_game = v["redistribute_in_game"].as_bool(false);
    l.notes = v["notes"].as_string_or("");
    return l;
}

Licence licence_cc0() {
    Licence l;
    l.id = "CC0-1.0";
    l.name = "CC0 1.0 Universal (public domain dedication)";
    l.url = "https://creativecommons.org/publicdomain/zero/1.0/";
    return l;
}

Licence licence_public_domain_us_gov() {
    Licence l;
    l.id = "LicenseRef-US-Government-Work";
    l.name = "Public domain (U.S. federal government work)";
    l.url = "https://www.usa.gov/government-copyright";
    l.notes = "Works of U.S. federal employees are not protected by copyright in the U.S. (17 U.S.C. 105). Credit the agency.";
    return l;
}

Licence licence_cc_by(std::string_view version) {
    Licence l;
    l.id = std::format("CC-BY-{}", version);
    l.name = std::format("Creative Commons Attribution {}", version);
    l.url = std::format("https://creativecommons.org/licenses/by/{}/", version);
    l.attribution_required = true;
    l.notes = "Credit the author, link the licence and say if you changed the asset. VICTUS writes this to CREDITS.md.";
    return l;
}

Licence licence_cc_by_sa(std::string_view version) {
    Licence l = licence_cc_by(version);
    l.id = std::format("CC-BY-SA-{}", version);
    l.name = std::format("Creative Commons Attribution-ShareAlike {}", version);
    l.url = std::format("https://creativecommons.org/licenses/by-sa/{}/", version);
    l.share_alike = true;
    l.notes = "Credit the author. Modified versions of this asset must be shared under the same licence.";
    return l;
}

Licence licence_from_cc_label(std::string_view label) {
    const std::string s = lower(label);
    const auto has = [&](std::string_view needle) { return s.find(needle) != std::string::npos; };
    if (s == "cc0" || has("cc0") || has("public domain")) return licence_cc0();
    const bool nc = has("noncommercial") || has("non-commercial") || has("-nc") || has("nc-") || s == "by-nc";
    const bool nd = has("noderiv") || has("no deriv") || has("-nd") || s == "by-nd";
    const bool sa = has("sharealike") || has("share alike") || has("-sa") || s == "by-sa";
    const bool by = has("attribution") || s.starts_with("by") || has("cc by") || has("cc-by");
    if (by && !nc && !nd) return sa ? licence_cc_by_sa() : licence_cc_by();
    Licence l;
    l.id = "LicenseRef-" + slugify(label);
    l.name = std::string(label);
    l.attribution_required = true;
    l.commercial_use = by && !nc;
    l.modification = by && !nd;
    l.share_alike = sa;
    l.redistribute_in_game = by && !nc && !nd;
    l.notes = nc ? "Non-commercial licence: VICTUS does not install it, because a game made with it could not be sold."
                 : nd ? "No-derivatives licence: VICTUS does not install it, because games modify and re-encode assets."
                      : "Unrecognised licence: VICTUS does not install it automatically. Check the asset page.";
    if (nc && nd) l.commercial_use = l.modification = false;
    return l;
}

std::string LibraryAsset::attribution() const {
    std::string who = author.empty() ? std::string{} : " by " + author;
    std::string text = std::format("\"{}\"{} ({}), licensed under {}", name, who, page_url, licence.name);
    if (!licence.url.empty()) text += " (" + licence.url + ")";
    return text;
}

json::Value LibraryAsset::to_json() const {
    json::Value v = json::Value::object();
    v.set("provider", provider);
    v.set("id", id);
    v.set("name", name);
    v.set("type", to_string(type));
    if (!category.empty()) v.set("category", category);
    json::Value t = json::Value::array();
    for (const auto& tag : tags) t.push(tag);
    v.set("tags", std::move(t));
    v.set("author", author);
    if (!description.empty()) v.set("description", description);
    v.set("page_url", page_url);
    if (!thumbnail_url.empty()) v.set("thumbnail_url", thumbnail_url);
    v.set("licence", licence.to_json());
    json::Value vars = json::Value::array();
    for (const auto& var : variants) {
        json::Value o = json::Value::object();
        o.set("id", var.id);
        o.set("label", var.label);
        if (var.bytes) o.set("bytes", var.bytes);
        vars.push(std::move(o));
    }
    v.set("variants", std::move(vars));
    if (requires_account) v.set("requires_account", true);
    if (!extra.is_null()) v.set("extra", extra);
    return v;
}

Result<LibraryAsset> LibraryAsset::from_json(const json::Value& v) {
    if (!v.is_object()) return make_error(ErrorCode::ParseError, "library asset: not an object");
    LibraryAsset a;
    a.provider = v["provider"].as_string_or("");
    a.id = v["id"].as_string_or("");
    if (a.provider.empty() || a.id.empty()) return make_error(ErrorCode::ParseError, "library asset: missing provider or id");
    a.name = v["name"].as_string_or(a.id);
    a.type = asset_type_from_string(v["type"].as_string_or("model"));
    a.category = v["category"].as_string_or("");
    for (const auto& t : v["tags"].as_array())
        if (t.is_string()) a.tags.push_back(t.as_string());
    a.author = v["author"].as_string_or("");
    a.description = v["description"].as_string_or("");
    a.page_url = v["page_url"].as_string_or("");
    a.thumbnail_url = v["thumbnail_url"].as_string_or("");
    if (!v["licence"].is_object()) return make_error(ErrorCode::ParseError, "library asset: missing licence");
    a.licence = Licence::from_json(v["licence"]);
    for (const auto& o : v["variants"].as_array())
        a.variants.push_back({o["id"].as_string_or(""), o["label"].as_string_or(""), static_cast<std::uint64_t>(o["bytes"].as_int(0))});
    a.requires_account = v["requires_account"].as_bool(false);
    if (const auto* e = v.find("extra")) a.extra = *e;
    return a;
}

std::vector<std::unique_ptr<Provider>> make_all_providers() {
    std::vector<std::unique_ptr<Provider>> out;
    out.push_back(make_polyhaven());
    out.push_back(make_ambientcg());
    out.push_back(make_kenney());
    out.push_back(make_smithsonian());
    out.push_back(make_sketchfab());
    out.push_back(make_bigsoundbank());
    out.push_back(make_nps_sounds());
    return out;
}

std::string user_agent() { return std::format("VICTUS-Studio/{} (+https://github.com/FAHTHIZ/VICTUS)", build::version()); }

std::string url_encode(std::string_view s) {
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(ch);
        else out += std::format("%{:02X}", c);
    }
    return out;
}

std::string html_unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        const std::size_t end = s.find(';', i);
        if (end == std::string_view::npos || end - i > 10) {
            out.push_back('&');
            continue;
        }
        const std::string_view ent = s.substr(i + 1, end - i - 1);
        std::string rep;
        if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos" || ent == "#39" || ent == "#039") rep = "'";
        else if (ent == "nbsp") rep = " ";
        else if (ent == "middot") rep = "\xC2\xB7";
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::stoul(std::string(ent.substr(2)), nullptr, 16)
                                                                         : std::stoul(std::string(ent.substr(1)));
            } catch (...) {
                cp = 0;
            }
            if (cp > 0 && cp < 0x80) rep = std::string(1, static_cast<char>(cp));
            else if (cp >= 0x80 && cp < 0x800) {
                rep += static_cast<char>(0xC0 | (cp >> 6));
                rep += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp >= 0x800 && cp < 0x10000) {
                rep += static_cast<char>(0xE0 | (cp >> 12));
                rep += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                rep += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        if (rep.empty()) {
            out.push_back('&');
            continue;
        }
        out += rep;
        i = end;
    }
    return out;
}

std::string slugify(std::string_view s) {
    // Keeps provider ids recognisable ("brown_mud_02", "nature-kit") while making names
    // ("City Kit (Roads)") safe folder names: lowercase ASCII, '-'/'_' kept, everything else -> '-'.
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(ch)));
        const bool sep_before = out.empty() || out.back() == '-' || out.back() == '_';
        if (std::isalnum(c) && c < 0x80) out.push_back(static_cast<char>(c));
        else if (c == '_' && !sep_before) out.push_back('_');
        else if (!sep_before) out.push_back('-');
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '_')) out.pop_back();
    if (out.size() > 80) out.resize(80);
    while (!out.empty() && (out.back() == '-' || out.back() == '_')) out.pop_back();
    return out.empty() ? std::string("asset") : out;
}

std::string format_bytes(std::uint64_t bytes) {
    if (bytes >= (1ull << 30)) return std::format("{:.1f} GB", static_cast<double>(bytes) / (1ull << 30));
    if (bytes >= (1ull << 20)) return std::format("{:.1f} MB", static_cast<double>(bytes) / (1ull << 20));
    if (bytes >= (1ull << 10)) return std::format("{:.1f} KB", static_cast<double>(bytes) / (1ull << 10));
    return std::format("{} B", bytes);
}

} // namespace victus::library
