// SPDX-License-Identifier: Apache-2.0
// ambientCG (https://ambientcg.com): CC0 PBR materials, decals and texture atlases through the
// public v2 API (https://ambientcg.com/api/v2/full_json). Licence (docs.ambientcg.com/license,
// checked 2026-11-21): CC0 1.0 for downloads and preview renders, no attribution required.
// ambientCG HDRIs ship as OpenEXR only, which the VICTUS texture importer does not read, so
// they are not offered here (Poly Haven provides .hdr environments).
#include "provider_util.hpp"

#include <format>

namespace victus::library {
namespace {

using namespace detail;

constexpr std::string_view kApi = "https://ambientcg.com/api/v2/full_json";

LibraryAsset to_asset(const json::Value& v) {
    LibraryAsset a;
    a.provider = "ambientcg";
    a.id = v["assetId"].as_string_or("");
    a.name = v["displayName"].as_string_or(a.id);
    a.type = AssetType::Material;
    a.category = v["displayCategory"].as_string_or(v["dataType"].as_string_or(""));
    for (const auto& t : v["tags"].as_array())
        if (t.is_string()) a.tags.push_back(t.as_string());
    a.author = "ambientCG (Lennart Demes)";
    a.description = v["dataTypeDescription"].as_string_or("");
    a.page_url = v["shortLink"].as_string_or("https://ambientcg.com/a/" + a.id);
    const json::Value& preview = v["previewImage"];
    a.thumbnail_url = preview["256-PNG"].as_string_or(preview["128-PNG"].as_string_or(""));
    a.licence = licence_cc0();
    a.licence.notes = "ambientCG assets are CC0. Suggested credit: \"Created using " + a.name + " from ambientCG.com\".";
    json::Value downloads = json::Value::object();
    for (const auto& [folder, fv] : v["downloadFolders"].as_object())
        for (const auto& [cat, cv] : fv["downloadFiletypeCategories"].as_object())
            for (const auto& d : cv["downloads"].as_array()) {
                const std::string attr = d["attribute"].as_string_or("");
                if (attr.find("JPG") == std::string::npos && attr.find("PNG") == std::string::npos) continue;
                const auto bytes = static_cast<std::uint64_t>(d["size"].as_int(0));
                a.variants.push_back({attr, std::format("{} ({})", attr, format_bytes(bytes)), bytes});
                json::Value o = json::Value::object();
                o.set("url", d["fullDownloadPath"].as_string_or(d["downloadLink"].as_string_or("")));
                o.set("size", bytes);
                downloads.set(attr, std::move(o));
            }
    json::Value extra = json::Value::object();
    extra.set("data_type", v["dataType"].as_string_or("Material"));
    extra.set("downloads", std::move(downloads));
    a.extra = std::move(extra);
    return a;
}

class AmbientCg final : public Provider {
public:
    AmbientCg() {
        info_.id = "ambientcg";
        info_.name = "ambientCG";
        info_.homepage = "https://ambientcg.com";
        info_.credit = "Materials from ambientCG.com (CC0)";
        info_.description = "CC0 PBR materials, decals and texture atlases with colour, normal, roughness, AO and height maps.";
        info_.types = {AssetType::Material};
    }
    const ProviderInfo& info() const override { return info_; }

    Result<SearchPage> search(HttpClient& http, const SearchQuery& q) override {
        if (!wants(q, AssetType::Material)) return SearchPage{};
        const std::uint32_t size = std::max<std::uint32_t>(1, q.page_size);
        std::string url = std::format("{}?type=Material,Decal,Atlas&sort=Popular&limit={}&offset={}&include=downloadData,previewData,tagData,displayData",
                                      kApi, size, static_cast<std::uint64_t>(q.page) * size);
        if (!q.text.empty()) url += "&q=" + url_encode(q.text);
        auto v = get_json(http, url);
        if (!v) return v.error();
        SearchPage page;
        for (const auto& e : v.value()["foundAssets"].as_array()) {
            LibraryAsset a = to_asset(e);
            if (!a.id.empty() && !a.variants.empty()) page.assets.push_back(std::move(a));
        }
        page.total = static_cast<std::uint64_t>(v.value()["numberOfResults"].as_int(static_cast<std::int64_t>(page.assets.size())));
        page.has_more = (static_cast<std::uint64_t>(q.page) + 1) * size < page.total;
        return page;
    }

    Result<InstallPlan> plan(HttpClient& /*http*/, const LibraryAsset& a, std::string_view variant) override {
        if (Status st = require_installable(a); !st) return st.error();
        const std::string attr = variant.empty() ? std::string("1K-JPG") : std::string(variant);
        const json::Value& d = a.extra["downloads"][attr];
        if (!d.is_object()) return make_error(ErrorCode::NotFound, "ambientCG has no " + attr + " download for " + a.id);
        InstallPlan plan;
        FileRequest zip;
        zip.url = d["url"].as_string_or("");
        zip.bytes = static_cast<std::uint64_t>(d["size"].as_int(0));
        zip.relative = "";
        zip.unzip = true;
        const std::string ext = attr.find("PNG") != std::string::npos ? "png" : "jpg";
        // Only the maps the material uses: the archive also carries a DirectX normal map and a
        // preview render, which would just be imported as stray textures.
        for (const char* m : {"color", "normalgl", "roughness", "metalness", "ambientocclusion", "displacement", "emission", "opacity"})
            zip.keep.push_back(std::format("_{}.{}", m, ext));
        plan.files.push_back(std::move(zip));
        const auto map = [&](std::string_view m) { return std::format("{}_{}_{}.{}", a.id, attr, m, ext); };
        plan.make_material = true;
        plan.material.base_color = map("Color");
        plan.material.normal = map("NormalGL");
        plan.material.roughness = map("Roughness");
        plan.material.metallic = map("Metalness");
        plan.material.occlusion = map("AmbientOcclusion");
        plan.material.height = map("Displacement");
        plan.material.emissive = map("Emission");
        plan.material.opacity = map("Opacity");
        plan.primary = plan.material.base_color;
        return plan;
    }

private:
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<Provider> make_ambientcg() { return std::make_unique<AmbientCg>(); }

} // namespace victus::library
