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
    case AssetKind::Audio: return "";  // decoded at runtime by the audio engine
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
    if (e == ".luau" || e == ".vgraph") return AssetKind::Script;
    if (e == ".vprefab") return AssetKind::Prefab;
    if (e == ".vmat") return AssetKind::Material;
    if (e == ".vshader" || e == ".vmatgraph") return AssetKind::Shader;
    if (e == ".vsheet") return AssetKind::SpriteSheet;
    if (e == ".vtileset") return AssetKind::Tileset;
    if (e == terrain::kTerrainExtension) return AssetKind::Terrain;
    if (e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac") return AssetKind::Audio;
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

Result<std::unique_ptr<AssetDatabase>> AssetDatabase::open_packaged(const fs::Path& project_root) {
    std::unique_ptr<AssetDatabase> db(new AssetDatabase(std::filesystem::absolute(project_root)));
    db->packaged_ = true;
    auto text = fs::read_text(db->index_path(), 64u << 20);
    if (!text) return make_error(ErrorCode::NotFound, std::format("not an exported game (no asset index): {}", text.error().message));
    auto doc = json::parse(*text);
    if (!doc) return std::move(doc).error();
    if ((*doc)["victus_asset_index"].as_int(0) != 1 || !(*doc)["assets"].is_array())
        return make_error(ErrorCode::VersionMismatch, "unsupported asset index version");
    for (const json::Value& a : (*doc)["assets"].as_array()) {
        AssetRecord r;
        auto path = fs::sanitize_relative(a["path"].as_string_or(""));
        if (!path || !Uuid::parse(a["uuid"].as_string_or(""), r.uuid) || r.uuid.is_nil())
            return make_error(ErrorCode::ParseError, "invalid asset index entry");
        r.path = *path;
        r.kind = kind_for_extension(fs::Path(r.path).extension().string());
        r.importer = a["importer"].as_string_or("");
        r.settings = a["settings"].is_object() ? a["settings"] : json::Value::object();
        if (db->by_uuid_.contains(r.uuid)) return make_error(ErrorCode::ParseError, "duplicate UUID in asset index");
        db->by_uuid_.emplace(r.uuid, db->records_.size());
        db->by_path_.emplace(r.path, db->records_.size());
        db->records_.push_back(std::move(r));
    }
    return db;
}

json::Value AssetDatabase::index_json() const {
    json::Value assets = json::Value::array();
    for (const AssetRecord& r : records()) {
        json::Value a = json::Value::object();
        a.set("uuid", r.uuid.to_string());
        a.set("path", r.path);
        a.set("importer", r.importer);
        a.set("settings", r.settings);
        assets.push(std::move(a));
    }
    json::Value doc = json::Value::object();
    doc.set("victus_asset_index", 1);
    doc.set("assets", std::move(assets));
    return doc;
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
    if (packaged_) return report;
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
    if (packaged_) return fs::exists(cooked_path(uuid));
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
    if (packaged_) {
        if (fs::exists(cooked_path(uuid))) return false;
        return make_error(ErrorCode::NotFound, std::format("{}: cooked data missing from the exported game", r.path));
    }
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
    if (packaged_) return {};
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
    if (!imported) {
        // Keep using the last good import (e.g. a source saved half-way while hot reloading).
        std::error_code ec;
        if (packaged_ || !std::filesystem::is_regular_file(cooked_path(uuid), ec)) return std::move(imported).error();
        VICTUS_LOG_WARN("assets", "{}; using the previous import", imported.error().message);
    }
    auto bytes = fs::read_binary(cooked_path(uuid));
    if (!bytes) return std::move(bytes).error();
    return deserialize_texture(*bytes);
}

Result<CookedModel> AssetDatabase::load_model(const Uuid& uuid) {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->importer != "gltf") return make_error(ErrorCode::NotFound, "no model asset " + uuid.to_string());
    auto imported = import(uuid);
    if (!imported) {
        // Keep using the last good import (e.g. a source saved half-way while hot reloading).
        std::error_code ec;
        if (packaged_ || !std::filesystem::is_regular_file(cooked_path(uuid), ec)) return std::move(imported).error();
        VICTUS_LOG_WARN("assets", "{}; using the previous import", imported.error().message);
    }
    auto bytes = fs::read_binary(cooked_path(uuid));
    if (!bytes) return std::move(bytes).error();
    return deserialize_model(*bytes);
}

Status AssetDatabase::set_settings(const Uuid& uuid, json::Value settings) {
    if (packaged_) return make_error(ErrorCode::Unsupported, "exported games are read-only");
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

animation::SetProvider animation_provider(AssetDatabase& db) {
    return [&db](const Uuid& uuid) -> std::shared_ptr<const animation::AnimationSet> {
        const AssetRecord* r = db.find(uuid);
        if (!r || r->kind != AssetKind::Model) return nullptr;
        auto model = db.load_model(uuid);
        if (!model || model->nodes.empty()) return nullptr;
        return std::make_shared<animation::AnimationSet>(make_animation_set(*model));
    };
}

} // namespace victus::assets

namespace victus::assets {

Result<MaterialAsset> AssetDatabase::load_material(const Uuid& uuid) const {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->kind != AssetKind::Material)
        return make_error(ErrorCode::NotFound, "material asset " + uuid.to_string() + " not found");
    return load_material_file(source_path(*rec));
}

Result<MaterialAsset> AssetDatabase::resolve_material(const Uuid& uuid) const {
    std::vector<MaterialAsset> chain; // leaf first
    std::vector<Uuid> ids;
    Uuid cur = uuid;
    for (;;) {
        if (std::find(ids.begin(), ids.end(), cur) != ids.end()) {
            std::string path;
            for (const Uuid& id : ids) {
                const AssetRecord* r = find(id);
                path += (r ? r->path : id.to_string()) + " -> ";
            }
            const AssetRecord* r = find(cur);
            return make_error(ErrorCode::InvalidArgument, "material inheritance cycle: " + path + (r ? r->path : cur.to_string()));
        }
        if (ids.size() >= kMaxMaterialDepth)
            return make_error(ErrorCode::LimitExceeded, std::format("material {} has more than {} parent levels", uuid.to_string(),
                                                                    kMaxMaterialDepth));
        auto m = load_material(cur);
        if (!m) {
            if (ids.empty()) return std::move(m).error();
            const AssetRecord* child = find(ids.back());
            return make_error(ErrorCode::NotFound, std::format("{}: parent material {} is missing ({})",
                                                               child ? child->path : ids.back().to_string(), cur.to_string(),
                                                               m.error().message));
        }
        ids.push_back(cur);
        const Uuid parent = m->parent;
        chain.push_back(std::move(m).value());
        if (parent.is_nil()) break;
        cur = parent;
    }
    MaterialAsset out = chain.back();
    for (std::size_t i = chain.size() - 1; i-- > 0;) out = apply_instance(out, chain[i]);
    return out;
}

Result<std::string> AssetDatabase::load_shader_source(const Uuid& uuid) const {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->kind != AssetKind::Shader)
        return make_error(ErrorCode::NotFound, "shader asset " + uuid.to_string() + " not found");
    return fs::read_text(source_path(*rec), 1u << 20);
}

Result<animation::SpriteSheet> AssetDatabase::load_sprite_sheet(const Uuid& uuid) const {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->kind != AssetKind::SpriteSheet)
        return make_error(ErrorCode::NotFound, "sprite sheet asset " + uuid.to_string() + " not found");
    const std::string path = rec->path; // records may move when the database refreshes
    auto sheet = animation::load_sprite_sheet_file(source_path(*rec));
    if (!sheet) return make_error(sheet.error().code, std::format("{}: {}", path, sheet.error().message));
    if (!sheet->texture.is_nil()) {
        const AssetRecord* tex = find(sheet->texture);
        if (!tex || tex->kind != AssetKind::Texture)
            return make_error(ErrorCode::NotFound, std::format("{}: texture {} is not a texture asset", path, sheet->texture.to_string()));
    }
    return sheet;
}

Result<tilemap::Tileset> AssetDatabase::load_tileset(const Uuid& uuid) const {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->kind != AssetKind::Tileset)
        return make_error(ErrorCode::NotFound, "tile set asset " + uuid.to_string() + " not found");
    const std::string path = rec->path;
    auto tileset = tilemap::load_tileset_file(source_path(*rec));
    if (!tileset) return make_error(tileset.error().code, std::format("{}: {}", path, tileset.error().message));
    if (!tileset->texture.is_nil()) {
        const AssetRecord* tex = find(tileset->texture);
        if (!tex || tex->kind != AssetKind::Texture)
            return make_error(ErrorCode::NotFound, std::format("{}: texture {} is not a texture asset", path, tileset->texture.to_string()));
    }
    return tileset;
}

Result<terrain::Heightfield> AssetDatabase::load_terrain(const Uuid& uuid) const {
    const AssetRecord* rec = find(uuid);
    if (!rec || rec->kind != AssetKind::Terrain)
        return make_error(ErrorCode::NotFound, "terrain asset " + uuid.to_string() + " not found");
    auto hf = terrain::load_file(source_path(*rec));
    if (!hf) return make_error(hf.error().code, std::format("{}: {}", rec->path, hf.error().message));
    return hf;
}

terrain::TerrainProvider terrain_provider(AssetDatabase& db) {
    return [&db](const Uuid& uuid) -> std::shared_ptr<terrain::Heightfield> {
        auto hf = db.load_terrain(uuid);
        if (!hf) {
            VICTUS_LOG_WARN("assets", "{}", hf.error().message);
            return nullptr;
        }
        return std::make_shared<terrain::Heightfield>(std::move(*hf));
    };
}

tilemap::TilesetProvider tileset_provider(AssetDatabase& db) {
    return [&db](const Uuid& uuid) -> std::shared_ptr<const tilemap::Tileset> {
        auto tileset = db.load_tileset(uuid);
        if (!tileset) {
            VICTUS_LOG_WARN("assets", "{}", tileset.error().message);
            return nullptr;
        }
        return std::make_shared<tilemap::Tileset>(std::move(*tileset));
    };
}

animation::SheetProvider sprite_sheet_provider(AssetDatabase& db) {
    return [&db](const Uuid& uuid) -> std::shared_ptr<const animation::SpriteSheet> {
        auto sheet = db.load_sprite_sheet(uuid);
        if (!sheet) {
            VICTUS_LOG_WARN("assets", "{}", sheet.error().message);
            return nullptr;
        }
        return std::make_shared<animation::SpriteSheet>(std::move(*sheet));
    };
}

} // namespace victus::assets
