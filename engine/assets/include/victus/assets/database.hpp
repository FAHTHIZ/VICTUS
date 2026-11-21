// SPDX-License-Identifier: Apache-2.0
// Project asset database.
//
// Layout of a VICTUS project:
//   <project>/assets/...            source files owned by the developer (versioned)
//   <project>/assets/foo.png.meta   sidecar: stable UUID, importer, import settings (versioned)
//   <project>/.victus/cache/        cooked artefacts, safe to delete (not versioned)
//
// UUIDs, not paths, are the identity of an asset: moving a file together with its .meta keeps
// every reference intact. A cooked entry is reused only when its header key (source bytes +
// importer id/version + settings) and all dependency hashes still match.
#pragma once
#include "victus/assets/cooked.hpp"
#include "victus/assets/material.hpp"
#include "victus/assets/importers.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/jobs.hpp"
#include "victus/physics/physics.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/terrain/terrain.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::assets {

inline constexpr int kMetaVersion = 1;

struct AssetRecord {
    Uuid uuid;
    std::string path; // relative to assets/, '/' separated
    AssetKind kind{AssetKind::Unknown};
    std::string importer; // "texture", "gltf", "scene", "" (copied verbatim / not imported)
    json::Value settings;
};

struct RefreshReport {
    std::vector<std::string> added;          // new sources that received a .meta
    std::vector<std::string> missing;        // .meta files whose source is gone
    std::vector<std::string> reassigned;     // duplicate UUIDs (copied files) given a fresh UUID
    std::vector<std::string> errors;         // unreadable/invalid .meta files (left untouched)
};

struct ImportReport {
    std::size_t imported{0};
    std::size_t up_to_date{0};
    std::size_t failed{0};
    std::vector<std::string> messages; // errors and warnings, prefixed with the asset path
};

[[nodiscard]] AssetKind kind_for_extension(std::string_view extension) noexcept;
// True for assets that are cooked into the cache (textures, models); others are used from source.
[[nodiscard]] bool is_importable(const AssetRecord& r) noexcept;

// Machine-wide cache of cooked assets shared by every project on this machine (ADR-0180), like a
// local derived-data cache: an import whose source bytes, importer, importer version and settings
// match an entry is satisfied by hard-linking (or copying) that entry instead of cooking again.
// Only self-contained results are shared (no external dependency files). `VICTUS_SHARED_CACHE`
// overrides the location, or disables the cache when set to "off"; the default is the user cache
// directory (`$XDG_CACHE_HOME/victus/cooked`, `~/Library/Caches/VICTUS/cooked`,
// `%LOCALAPPDATA%\VICTUS\Cache\cooked`). `VICTUS_SHARED_CACHE_MB` caps its size (default 8192 MB;
// least recently used entries go first). Returns an empty path when disabled.
[[nodiscard]] fs::Path shared_cooked_cache_dir();

class AssetDatabase {
public:
    // Opens (and creates assets/ and .victus/cache/ if missing) and runs refresh().
    [[nodiscard]] static Result<std::unique_ptr<AssetDatabase>> open(const fs::Path& project_root,
                                                                     RefreshReport* report = nullptr);

    // Opens an exported game: records come from `.victus/assets.index.json` (written by
    // index_json() at export time), nothing is scanned, imported or written, and cooked data is
    // read as-is. Works on loose folders and on packs mounted at `project_root` (fs::mount).
    [[nodiscard]] static Result<std::unique_ptr<AssetDatabase>> open_packaged(const fs::Path& project_root);
    [[nodiscard]] bool packaged() const noexcept { return packaged_; }
    // Serializes every record (uuid, path, kind, importer, settings) for open_packaged().
    [[nodiscard]] json::Value index_json() const;
    [[nodiscard]] fs::Path index_path() const { return root_ / ".victus" / "assets.index.json"; }

    // Rescans assets/, creating .meta files for new sources. No-op for packaged databases.
    [[nodiscard]] RefreshReport refresh();
    // Imports every importable asset whose cooked entry is stale. Parallel when jobs != null.
    [[nodiscard]] ImportReport import_all(jobs::JobSystem* jobs = nullptr);
    // Imports one asset if stale. Returns true when an import actually ran.
    [[nodiscard]] Result<bool> import(const Uuid& uuid, std::vector<std::string>* messages = nullptr);
    [[nodiscard]] Result<bool> is_up_to_date(const Uuid& uuid) const;

    [[nodiscard]] const AssetRecord* find(const Uuid& uuid) const;
    [[nodiscard]] const AssetRecord* find_path(std::string_view relative) const;
    [[nodiscard]] std::vector<AssetRecord> records() const;

    // Loads cooked data, importing first when stale.
    [[nodiscard]] Result<CookedTexture> load_texture(const Uuid& uuid);
    [[nodiscard]] Result<CookedModel> load_model(const Uuid& uuid);
    // One embedded texture of a model (index into CookedModel::textures) without its meshes.
    [[nodiscard]] Result<CookedTexture> load_model_texture(const Uuid& uuid, std::uint32_t index);
    // Material assets (.vmat) are read from source; texture references are not resolved here.
    [[nodiscard]] Result<MaterialAsset> load_material(const Uuid& uuid) const;
    // The material with its parent chain applied (instances inherit unset fields). Fails on a
    // missing parent, a cycle or a chain deeper than kMaxMaterialDepth.
    [[nodiscard]] Result<MaterialAsset> resolve_material(const Uuid& uuid) const;
    // Source text of a surface shader asset (.vshader).
    [[nodiscard]] Result<std::string> load_shader_source(const Uuid& uuid) const;
    // A sprite sheet asset (.vsheet), validated (its texture must be a texture asset).
    [[nodiscard]] Result<animation::SpriteSheet> load_sprite_sheet(const Uuid& uuid) const;
    // A tile set asset (.vtileset), validated (its texture must be a texture asset).
    [[nodiscard]] Result<tilemap::Tileset> load_tileset(const Uuid& uuid) const;
    // Loads a .vterrain heightfield (source file, not cooked).
    [[nodiscard]] Result<terrain::Heightfield> load_terrain(const Uuid& uuid) const;

    // Changes import settings (writes the .meta); the next import re-cooks.
    [[nodiscard]] Status set_settings(const Uuid& uuid, json::Value settings);

    [[nodiscard]] const fs::Path& project_root() const noexcept { return root_; }
    [[nodiscard]] fs::Path assets_dir() const { return root_ / "assets"; }
    [[nodiscard]] fs::Path cache_dir() const { return root_ / ".victus" / "cache"; }
    [[nodiscard]] fs::Path source_path(const AssetRecord& r) const;
    [[nodiscard]] fs::Path cooked_path(const Uuid& uuid) const;
    // Imports satisfied from the shared cooked-data cache (see shared_cooked_cache_dir) since open.
    [[nodiscard]] std::uint32_t shared_cache_hits() const noexcept { return shared_hits_.load(); }

private:
    explicit AssetDatabase(fs::Path root) : root_(std::move(root)) {}
    [[nodiscard]] Result<std::uint64_t> source_key(const AssetRecord& r) const;
    [[nodiscard]] Status write_meta(const AssetRecord& r) const;
    // Writes a new sidecar only if none exists; false when another writer created it first.
    [[nodiscard]] Result<bool> create_meta(const AssetRecord& r) const;

    fs::Path root_;
    bool packaged_{false};
    std::atomic<std::uint32_t> shared_hits_{0};
    mutable std::mutex mutex_;
    std::vector<AssetRecord> records_;
    std::unordered_map<Uuid, std::size_t, UuidHash> by_uuid_;
    std::unordered_map<std::string, std::size_t> by_path_;
};

// Animation sets for the animation system, loaded from cooked models in `db` (which must outlive
// the provider). Returns null for unknown assets, non-models and models without nodes.
[[nodiscard]] animation::SetProvider animation_provider(AssetDatabase& db);
// Sheets for SpriteAnimator (AnimationSettings::sheets); invalid sheets resolve to null (logged).
[[nodiscard]] animation::SheetProvider sprite_sheet_provider(AssetDatabase& db);
// Tile sets for tilemap::TilemapSystem; invalid tile sets resolve to null (logged).
[[nodiscard]] tilemap::TilesetProvider tileset_provider(AssetDatabase& db);
[[nodiscard]] terrain::TerrainProvider terrain_provider(AssetDatabase& db);
// Convex/mesh collider geometry (physics::PhysicsWorld::set_mesh_provider): LOD 0 positions and
// indices of a cooked model mesh, or a MeshRenderer primitive generated exactly as the renderer
// draws it. Unknown assets, non-models and out-of-range mesh indices resolve to null (logged).
[[nodiscard]] physics::CollisionMeshProvider collision_mesh_provider(AssetDatabase& db);
// Collision geometry of a CPU mesh (LOD 0, no morph or skin deformation).
[[nodiscard]] std::shared_ptr<const physics::CollisionMesh> collision_mesh(const render::MeshData& mesh);
// The geometry of a MeshRenderer primitive (cube|sphere|plane|cylinder), or null for other names.
[[nodiscard]] std::shared_ptr<const physics::CollisionMesh> primitive_collision_mesh(std::string_view name);

} // namespace victus::assets
