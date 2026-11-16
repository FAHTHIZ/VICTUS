// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/database.hpp"

#include "victus/core/log.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <set>

namespace victus::assets {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string default_importer(AssetKind kind) {
    switch (kind) {
    case AssetKind::Texture: return "texture";
    case AssetKind::Model: return "gltf";
    case AssetKind::Scene: return "scene";
    case AssetKind::Script: return ""; // used directly from source, nothing to cook
    default: return "";
    }
}

bool importable(const AssetRecord& r) { return r.importer == "texture" || r.importer == "gltf"; }

std::uint32_t importer_version(const std::string& importer) {
    return importer == "texture" ? kTextureImporterVersion : importer == "gltf" ? kModelImporterVersion : 0;
}

// Linear-data textures by naming convention (normal, ORM, roughness...). Overridable in .meta.
bool looks_linear(std::string_view relative) {
    std::string stem = lower(fs::Path(std::string(relative)).stem().string());
    for (const char* suffix : {"_n", "_nrm", "_normal", "_normals", "_orm", "_mr", "_rough", "_roughness", "_metal",
                               "_metallic", "_metalness", "_ao", "_occlusion", "_height", "_mask", "_data"}) {
        if (stem.ends_with(suffix)) return true;
    }
    return false;
}

json::Value default_settings(const std::string& importer, std::string_view relative) {
    if (importer == "texture") {
        TextureImportSettings s;
        s.srgb = !looks_linear(relative);
        return s.to_json();
    }
    if (importer == "gltf") return ModelImportSettings{}.to_json();
    return json::Value::object();
}

Result<std::vector<std::uint8_t>> read_prefix(const fs::Path& p, std::size_t max) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return make_error(ErrorCode::NotFound, p.string());
    std::vector<std::uint8_t> bytes(max);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(max));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    return bytes;
}

Result<std::uint64_t> hash_file(const fs::Path& p) {
    auto bytes = fs::read_binary(p);
    if (!bytes) return std::move(bytes).error();
    return fnv1a64(*bytes);
}

} // namespace

AssetKind kind_for_extension(std::string_view extension) noexcept {
    std::string e;
    for (const char c : extension) e.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".hdr") return AssetKind::Texture;
    if (e == ".gltf" || e == ".glb") return AssetKind::Model;
    if (e == ".vscene") return AssetKind::Scene;
    if (e == ".luau") return AssetKind::Script;
    return AssetKind::Unknown;
}

Result<std::unique_ptr<AssetDatabase>> AssetDatabase::open(const fs::Path& project_root, RefreshReport* report) {
    std::error_code ec;
    if (!std::filesystem::is_directory(project_root, ec))
        return make_error(ErrorCode::NotFound, std::format("project directory not found: {}", project_root.string()));
    std::unique_ptr<AssetDatabase> db(new AssetDatabase(std::filesystem::absolute(project_root)));
    std::filesystem::create_directories(db->assets_dir(), ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", db->assets_dir().string(), ec.message()));
    std::filesystem::create_directories(db->cache_dir(), ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", db->cache_dir().string(), ec.message()));
    RefreshReport r = db->refresh();
    for (const auto& e : r.errors) VICTUS_LOG_WARN("assets", "{}", e);
    if (report) *report = std::move(r);
    return db;
}

fs::Path AssetDatabase::source_path(const AssetRecord& r) const { return assets_dir() / fs::Path(r.path); }

fs::Path AssetDatabase::cooked_path(const Uuid& uuid) const { return cache_dir() / (uuid.to_string() + ".vcooked"); }

Status AssetDatabase::write_meta(const AssetRecord& r) const {
    json::Value meta = json::Value::object();
    meta.set("victus_meta", kMetaVersion);
    meta.set("uuid", r.uuid.to_string());
    meta.set("importer", r.importer);
    meta.set("settings", r.settings);
    return fs::write_text_atomic(fs::Path(source_path(r).string() + ".meta"), json::stringify(meta, true) + "\n");
}

RefreshReport AssetDatabase::refresh() {
    RefreshReport report;
    std::vector<std::string> sources;
    std::set<std::string> metas;
    const fs::Path base = assets_dir();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const fs::Path& p = it->path();
        const std::string name = p.filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        const std::string rel = std::filesystem::relative(p, base).generic_string();
        if (p.extension() == ".meta") metas.insert(rel.substr(0, rel.size() - 5));
        else sources.push_back(rel);
    }
    std::sort(sources.begin(), sources.end());

    std::vector<AssetRecord> records;
    std::unordered_map<Uuid, std::size_t, UuidHash> by_uuid;
    for (const std::string& rel : sources) {
        AssetRecord r;
        r.path = rel;
        r.kind = kind_for_extension(fs::Path(rel).extension().string());
        const fs::Path meta_path = fs::Path((base / fs::Path(rel)).string() + ".meta");
        bool write = false;
        if (metas.erase(rel)) {
            auto text = fs::read_text(meta_path, 1u << 20);
            auto doc = text ? json::parse(*text) : Result<json::Value>(std::move(text).error());
            if (!doc || !doc->is_object() || !Uuid::parse((*doc)["uuid"].as_string_or(""), r.uuid) || r.uuid.is_nil()) {
                report.errors.push_back(std::format("{}.meta is invalid; asset ignored until fixed", rel));
                continue;
            }
            r.importer = (*doc)["importer"].as_string_or(default_importer(r.kind));
            r.settings = (*doc)["settings"].is_object() ? (*doc)["settings"] : default_settings(r.importer, rel);
        } else {
            r.uuid = Uuid::generate();
            r.importer = default_importer(r.kind);
            r.settings = default_settings(r.importer, rel);
            write = true;
            report.added.push_back(rel);
        }
        if (by_uuid.contains(r.uuid)) {
            // A copied file+meta pair: the later path (sorted order) gets a fresh identity.
            r.uuid = Uuid::generate();
            write = true;
            report.reassigned.push_back(rel);
        }
        if (write) {
            if (auto st = write_meta(r); !st.ok()) report.errors.push_back(std::format("{}: {}", rel, st.error().message));
        }
        by_uuid.emplace(r.uuid, records.size());
        records.push_back(std::move(r));
    }
    report.missing.assign(metas.begin(), metas.end());

    std::lock_guard lock(mutex_);
    records_ = std::move(records);
    by_uuid_ = std::move(by_uuid);
    by_path_.clear();
    for (std::size_t i = 0; i < records_.size(); ++i) by_path_.emplace(records_[i].path, i);
    return report;
}

const AssetRecord* AssetDatabase::find(const Uuid& uuid) const {
    std::lock_guard lock(mutex_);
    const auto it = by_uuid_.find(uuid);
    return it == by_uuid_.end() ? nullptr : &records_[it->second];
}

const AssetRecord* AssetDatabase::find_path(std::string_view relative) const {
    std::lock_guard lock(mutex_);
    const auto it = by_path_.find(std::string(relative));
    return it == by_path_.end() ? nullptr : &records_[it->second];
}

std::vector<AssetRecord> AssetDatabase::records() const {
    std::lock_guard lock(mutex_);
    return records_;
}

Result<std::uint64_t> AssetDatabase::source_key(const AssetRecord& r) const {
    auto bytes = fs::read_binary(source_path(r));
    if (!bytes) return std::move(bytes).error();
    const std::string salt = std::format("{}/{}/{}", r.importer, importer_version(r.importer), json::stringify(r.settings, false));
    return fnv1a64(*bytes, fnv1a64(salt));
}

Result<bool> AssetDatabase::is_up_to_date(const Uuid& uuid) const {
    const AssetRecord* rec = find(uuid);
    if (!rec) return make_error(ErrorCode::NotFound, "unknown asset " + uuid.to_string());
    const AssetRecord r = *rec;
    if (!importable(r)) return true;
    auto prefix = read_prefix(cooked_path(uuid), 1u << 20);
    if (!prefix) return false;
    auto header = read_header(*prefix);
    if (!header) return false;
    auto key = source_key(r);
    if (!key) return std::move(key).error();
    if (header->key != *key) return false;
    for (const auto& dep : header->dependencies) {
        auto h = hash_file(root_ / fs::Path(dep.path));
        if (!h || *h != dep.hash) return false;
    }
    return true;
}

Result<bool> AssetDatabase::import(const Uuid& uuid, std::vector<std::string>* messages) {
    const AssetRecord* rec = find(uuid);
    if (!rec) return make_error(ErrorCode::NotFound, "unknown asset " + uuid.to_string());
    const AssetRecord r = *rec;
    if (!importable(r)) return false;
    auto fresh = is_up_to_date(uuid);
    if (!fresh) return std::move(fresh).error();
    if (*fresh) return false;

    auto key = source_key(r);
    if (!key) return std::move(key).error();
    CookedHeader header;
    header.key = *key;
    std::vector<std::uint8_t> bytes;
    const fs::Path src = source_path(r);
    if (r.importer == "texture") {
        auto tex = import_texture(src, TextureImportSettings::from_json(r.settings));
        if (!tex) return make_error(tex.error().code, std::format("{}: {}", r.path, tex.error().message));
        bytes = serialize(header, *tex);
    } else {
        ImportResult result;
        auto model = import_gltf(src, ModelImportSettings::from_json(r.settings), assets_dir(), &result);
        if (messages)
            for (const auto& w : result.warnings) messages->push_back(std::format("{}: warning: {}", r.path, w));
        if (!model) return make_error(model.error().code, std::format("{}: {}", r.path, model.error().message));
        for (const auto& dep : result.dependencies) {
            auto h = hash_file(dep);
            if (!h) return std::move(h).error();
            header.dependencies.push_back(
                {std::filesystem::relative(std::filesystem::weakly_canonical(dep), std::filesystem::weakly_canonical(root_))
                     .generic_string(),
                 *h});
        }
        bytes = serialize(header, *model);
    }
    if (auto st = fs::write_atomic(cooked_path(uuid), bytes); !st.ok()) return std::move(st).error();
    return true;
}

ImportReport AssetDatabase::import_all(jobs::JobSystem* jobs) {
    std::vector<Uuid> todo;
    for (const auto& r : records())
        if (importable(r)) todo.push_back(r.uuid);
    struct Outcome {
        int state{0}; // 0 up to date, 1 imported, 2 failed
        std::vector<std::string> messages;
    };
    std::vector<Outcome> outcomes(todo.size());
    auto work = [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) {
            auto res = import(todo[i], &outcomes[i].messages);
            if (!res) {
                outcomes[i].state = 2;
                outcomes[i].messages.push_back(res.error().message);
            } else {
                outcomes[i].state = *res ? 1 : 0;
            }
        }
    };
    if (jobs && todo.size() > 1) jobs->parallel_for(0, todo.size(), 1, work);
    else work(0, todo.size());
    ImportReport report;
    for (auto& o : outcomes) {
        if (o.state == 0) ++report.up_to_date;
        else if (o.state == 1) ++report.imported;
        else ++report.failed;
        for (auto& m : o.messages) report.messages.push_back(std::move(m));
    }
    return report;
}

Result<CookedTexture> AssetDatabase::load_texture(const Uuid& uuid) {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->importer != "texture") return make_error(ErrorCode::NotFound, "no texture asset " + uuid.to_string());
    auto imported = import(uuid);
    if (!imported) return std::move(imported).error();
    auto bytes = fs::read_binary(cooked_path(uuid));
    if (!bytes) return std::move(bytes).error();
    return deserialize_texture(*bytes);
}

Result<CookedModel> AssetDatabase::load_model(const Uuid& uuid) {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->importer != "gltf") return make_error(ErrorCode::NotFound, "no model asset " + uuid.to_string());
    auto imported = import(uuid);
    if (!imported) return std::move(imported).error();
    auto bytes = fs::read_binary(cooked_path(uuid));
    if (!bytes) return std::move(bytes).error();
    return deserialize_model(*bytes);
}

Status AssetDatabase::set_settings(const Uuid& uuid, json::Value settings) {
    AssetRecord copy;
    {
        std::lock_guard lock(mutex_);
        const auto it = by_uuid_.find(uuid);
        if (it == by_uuid_.end()) return make_error(ErrorCode::NotFound, "unknown asset " + uuid.to_string());
        records_[it->second].settings = std::move(settings);
        copy = records_[it->second];
    }
    return write_meta(copy);
}

} // namespace victus::assets
