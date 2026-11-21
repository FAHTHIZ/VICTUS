// SPDX-License-Identifier: Apache-2.0
// Poly Haven (https://polyhaven.com): CC0 HDRIs, PBR textures and 3D models through the public
// API (https://api.polyhaven.com). API terms (Poly-Haven/Public-API ToS.md, checked 2026-11-21):
// free for any use, requests must carry a User-Agent naming the software, and software that
// surfaces content from the live API shows a visible "Poly Haven" credit near it.
#include "provider_util.hpp"

#include <algorithm>
#include <format>
#include <mutex>

namespace victus::library {
namespace {

using namespace detail;

constexpr std::string_view kApi = "https://api.polyhaven.com";

AssetType type_of(int t) { return t == 0 ? AssetType::Hdri : t == 1 ? AssetType::Material : AssetType::Model; }
const char* api_type(AssetType t) { return t == AssetType::Hdri ? "hdris" : t == AssetType::Material ? "textures" : "models"; }

LibraryAsset to_asset(const std::string& slug, const json::Value& v) {
    LibraryAsset a;
    a.provider = "polyhaven";
    a.id = slug;
    a.name = v["name"].as_string_or(slug);
    a.type = type_of(static_cast<int>(v["type"].as_int(2)));
    for (const auto& c : v["categories"].as_array())
        if (c.is_string()) {
            if (a.category.empty()) a.category = c.as_string();
            a.tags.push_back(c.as_string());
        }
    for (const auto& t : v["tags"].as_array())
        if (t.is_string()) a.tags.push_back(t.as_string());
    std::string authors;
    for (const auto& [name, role] : v["authors"].as_object()) authors += (authors.empty() ? "" : ", ") + name;
    a.author = authors;
    a.page_url = "https://polyhaven.com/a/" + slug;
    a.thumbnail_url = "https://cdn.polyhaven.com/asset_img/thumbs/" + slug + ".png?width=256&height=256";
    a.licence = licence_cc0();
    a.licence.notes = "Poly Haven assets are CC0: no attribution is required (credit is appreciated).";
    a.variants = {{"1k", "1K", 0}, {"2k", "2K", 0}, {"4k", "4K", 0}};
    json::Value extra = json::Value::object();
    extra.set("downloads", v["download_count"].as_number(0));
    a.extra = std::move(extra);
    return a;
}

class PolyHaven final : public Provider {
public:
    PolyHaven() {
        info_.id = "polyhaven";
        info_.name = "Poly Haven";
        info_.homepage = "https://polyhaven.com";
        info_.credit = "Powered by Poly Haven";
        info_.description = "CC0 HDRIs, scanned PBR materials and photoreal 3D models.";
        info_.types = {AssetType::Model, AssetType::Material, AssetType::Hdri};
    }
    const ProviderInfo& info() const override { return info_; }

    Result<SearchPage> search(HttpClient& http, const SearchQuery& q) override {
        std::vector<AssetType> types;
        for (const AssetType t : info_.types)
            if (wants(q, t)) types.push_back(t);
        if (types.empty()) return SearchPage{};
        std::vector<LibraryAsset> all;
        for (const AssetType t : types) {
            auto index = assets_index(http, t);
            if (!index) return index.error();
            const json::Value& data = index.value();
            if (!q.text.empty()) {
                // The API's own ranked search (semantic + keyword), then the metadata from the index.
                auto found = get_json(http, std::format("{}/search?q={}&t={}", kApi, url_encode(q.text), api_type(t)));
                if (!found) return found.error();
                for (const auto& r : found.value()["results"].as_array()) {
                    const std::string slug = r["slug"].as_string_or("");
                    if (const json::Value* v = data.find(slug)) all.push_back(to_asset(slug, *v));
                }
            } else {
                for (const auto& [slug, v] : data.as_object()) all.push_back(to_asset(slug, v));
                std::stable_sort(all.begin(), all.end(), [](const LibraryAsset& x, const LibraryAsset& y) {
                    return x.extra["downloads"].as_number() > y.extra["downloads"].as_number();
                });
            }
        }
        return paginate(std::move(all), q);
    }

    Result<InstallPlan> plan(HttpClient& http, const LibraryAsset& a, std::string_view variant) override {
        if (Status st = require_installable(a); !st) return st.error();
        const std::string res = variant.empty() ? std::string("1k") : std::string(variant);
        auto files = get_json(http, std::format("{}/files/{}", kApi, url_encode(a.id)));
        if (!files) return files.error();
        const json::Value& f = files.value();
        InstallPlan plan;
        const auto add = [&](const json::Value& entry, std::string relative) {
            FileRequest r;
            r.url = entry["url"].as_string_or("");
            r.bytes = static_cast<std::uint64_t>(entry["size"].as_int(0));
            r.md5 = entry["md5"].as_string_or("");
            r.relative = std::move(relative);
            plan.files.push_back(std::move(r));
        };
        if (a.type == AssetType::Hdri) {
            const json::Value& e = f["hdri"][res]["hdr"];
            if (!e.is_object()) return make_error(ErrorCode::NotFound, "Poly Haven has no " + res + " HDR for " + a.id);
            add(e, a.id + "_" + res + ".hdr");
            plan.primary = plan.files.back().relative;
            return plan;
        }
        if (a.type == AssetType::Model) {
            const json::Value& g = f["gltf"][res]["gltf"];
            if (!g.is_object()) return make_error(ErrorCode::NotFound, "Poly Haven has no " + res + " glTF for " + a.id);
            add(g, a.id + ".gltf");
            plan.primary = plan.files.back().relative;
            for (const auto& [rel, inc] : g["include"].as_object()) add(inc, rel);
            return plan;
        }
        // Textures: colour, OpenGL normal, packed AO/rough/metal (Poly Haven's "arm" is the glTF
        // ORM layout) and displacement.
        const auto map = [&](std::string_view key) -> const json::Value& {
            const json::Value& m = f[key][res];
            return m["jpg"].is_object() ? m["jpg"] : m["png"];
        };
        const auto add_map = [&](std::string_view key, std::string_view suffix, std::string* slot) {
            const json::Value& e = map(key);
            if (!e.is_object()) return;
            add(e, std::format("{}_{}_{}.{}", a.id, suffix, res, url_extension(e["url"].as_string_or(".jpg"))));
            *slot = plan.files.back().relative;
        };
        plan.make_material = true;
        add_map("Diffuse", "color", &plan.material.base_color);
        add_map("nor_gl", "normal", &plan.material.normal);
        std::string arm;
        add_map("arm", "orm", &arm);
        if (arm.empty()) {
            add_map("AO", "ao", &plan.material.occlusion);
            add_map("Rough", "roughness", &plan.material.roughness);
            add_map("Metal", "metallic", &plan.material.metallic);
        } else {
            plan.material.occlusion = plan.material.roughness = plan.material.metallic = arm; // already packed
        }
        add_map("Displacement", "height", &plan.material.height);
        if (plan.material.base_color.empty()) return make_error(ErrorCode::NotFound, "Poly Haven has no " + res + " colour map for " + a.id);
        plan.primary = plan.material.base_color;
        return plan;
    }

private:
    Result<json::Value> assets_index(HttpClient& http, AssetType t) {
        std::lock_guard lock(mutex_);
        json::Value& slot = index_[static_cast<std::size_t>(t)];
        if (!slot.is_null()) return slot;
        auto v = get_json(http, std::format("{}/assets?type={}", kApi, api_type(t)));
        if (!v) return v.error();
        if (!v->is_object()) return make_error(ErrorCode::ParseError, "Poly Haven: unexpected asset index");
        slot = std::move(v.value());
        return slot;
    }

    ProviderInfo info_;
    std::mutex mutex_;
    json::Value index_[static_cast<std::size_t>(AssetType::Count)];
};

} // namespace

std::unique_ptr<Provider> make_polyhaven() { return std::make_unique<PolyHaven>(); }

} // namespace victus::library
