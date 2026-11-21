// SPDX-License-Identifier: Apache-2.0
// Smithsonian Open Access 3D (https://3d.si.edu) through the keyless 3D API
// (https://3d-api.si.edu/api/v1.0/content/file/search). The API documentation states every file
// it serves is released under Smithsonian Open Access, which dedicates content to the public
// domain with CC0 (https://www.si.edu/openaccess/faq, checked 2026-11-21). The FAQ notes that
// trademarks, privacy and publicity rights of depicted people or brands are not covered, which
// Studio shows with the licence.
//
// The search returns one row per file; files of the same model share `model_url`. VICTUS installs
// the "Download3D" GLB that is not Draco-compressed (the importer has no Draco decoder).
#include "provider_util.hpp"

#include <format>
#include <unordered_map>

namespace victus::library {
namespace {

using namespace detail;

constexpr std::string_view kApi = "https://3d-api.si.edu/api/v1.0/content/file/search";
constexpr std::uint32_t kRowsPerModel = 16; // a model has ~12-16 file rows

bool is_draco(const json::Value& c) {
    return c["draco_compressed"].as_bool(false) || c["uri"].as_string_or("").find("draco") != std::string::npos ||
           c["file_type"].as_string_or("").find("draco") != std::string::npos;
}

class Smithsonian final : public Provider {
public:
    Smithsonian() {
        info_.id = "smithsonian";
        info_.name = "Smithsonian Open Access";
        info_.homepage = "https://3d.si.edu";
        info_.credit = "3D scans courtesy of the Smithsonian Institution (Open Access, CC0)";
        info_.description = "CC0 photogrammetry scans of museum objects, fossils, artefacts and specimens.";
        info_.types = {AssetType::Model};
    }
    const ProviderInfo& info() const override { return info_; }

    Result<SearchPage> search(HttpClient& http, const SearchQuery& q) override {
        if (!wants(q, AssetType::Model)) return SearchPage{};
        const std::uint32_t size = std::max<std::uint32_t>(1, q.page_size);
        const std::uint32_t rows = size * kRowsPerModel;
        const std::string url = std::format("{}?q={}&start={}&rows={}", kApi, url_encode(q.text.empty() ? "*" : q.text),
                                            static_cast<std::uint64_t>(q.page) * rows, rows);
        auto v = get_json(http, url);
        if (!v) return v.error();
        // Group file rows by model, keeping first-seen order.
        std::vector<LibraryAsset> models;
        std::unordered_map<std::string, std::size_t> index;
        for (const auto& r : v.value()["rows"].as_array()) {
            const json::Value& c = r["content"];
            const std::string model = c["model_url"].as_string_or("");
            if (model.empty()) continue;
            auto [it, inserted] = index.try_emplace(model, models.size());
            if (inserted) {
                LibraryAsset a;
                a.provider = "smithsonian";
                a.id = model.substr(model.find(':') == std::string::npos ? 0 : model.find(':') + 1);
                a.name = r["title"].as_string_or("Smithsonian scan");
                a.type = AssetType::Model;
                a.category = "Museum scan";
                a.author = "Smithsonian Institution";
                a.page_url = "https://3d-api.si.edu/voyager/" + model; // documented embed viewer
                a.licence = licence_cc0();
                a.licence.name = "Smithsonian Open Access (CC0 1.0)";
                a.licence.notes = "Released through the Smithsonian Open Access initiative (CC0). Trademarks and the privacy or publicity rights of "
                                  "anything depicted are not covered by the dedication.";
                a.extra = json::Value::object();
                models.push_back(std::move(a));
            }
            LibraryAsset& a = models[it->second];
            const std::string usage = c["usage"].as_string_or("");
            const std::string uri = c["uri"].as_string_or("");
            if (usage == "Image2D" && c["quality"].as_string_or("") == "Thumb") a.thumbnail_url = uri;
            if (usage == "Download3D" && c["file_type"].as_string_or("") == "glb" && !is_draco(c) && !a.extra.find("glb"))
                a.extra.set("glb", uri);
        }
        SearchPage page;
        for (LibraryAsset& a : models)
            if (a.extra.find("glb")) {
                a.variants = {{"glb", "GLB (game resolution)", 0}};
                page.assets.push_back(std::move(a));
            }
        const auto row_count = static_cast<std::uint64_t>(v.value()["rowCount"].as_int(0));
        page.total = row_count / kRowsPerModel;
        page.has_more = (static_cast<std::uint64_t>(q.page) + 1) * rows < row_count;
        return page;
    }

    Result<InstallPlan> plan(HttpClient& /*http*/, const LibraryAsset& a, std::string_view /*variant*/) override {
        if (Status st = require_installable(a); !st) return st.error();
        const std::string uri = a.extra["glb"].as_string_or("");
        if (uri.empty()) return make_error(ErrorCode::NotFound, "no downloadable GLB for " + a.name);
        InstallPlan plan;
        FileRequest f;
        f.url = uri;
        f.relative = slugify(a.name) + ".glb";
        plan.files.push_back(std::move(f));
        plan.primary = plan.files.back().relative;
        return plan;
    }

private:
    ProviderInfo info_;
};

} // namespace

std::unique_ptr<Provider> make_smithsonian() { return std::make_unique<Smithsonian>(); }

} // namespace victus::library
