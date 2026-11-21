// SPDX-License-Identifier: Apache-2.0
// Installs Asset Store content into a project (ADR-0174).
//
//   1. The asset's licence is checked (Licence::installable) before anything is fetched.
//   2. Files are downloaded into <project>/.victus/library_staging/<key>/ (size-capped, md5
//      checked when the provider publishes one, archives extracted with path checks).
//   3. Texture sets get a packed ORM texture (R occlusion, G roughness, B metallic) and a .vmat.
//   4. The staged folder is renamed into assets/library/<provider>/<slug>/ in one step, so a
//      failed or cancelled install never leaves a half-imported asset visible.
//   5. LICENSE.txt and provenance.json are written next to the files, CREDITS.md at the project
//      root is regenerated from every provenance record, and the AssetDatabase is refreshed.
//
// Installed assets are ordinary project files with project-local UUIDs: the project keeps
// working, and exports, after the user-level store cache is deleted or the network is gone.
#pragma once
#include "victus/assets/database.hpp"
#include "victus/library/library.hpp"

#include <atomic>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace victus::library {

inline constexpr std::string_view kLibraryFolder = "library";       // under assets/
inline constexpr std::string_view kProvenanceFile = "provenance.json";
inline constexpr int kProvenanceVersion = 1;

struct InstallProgress {
    std::string stage;      // "Downloading 2/5: Bricks_Color.jpg"
    float fraction{0.0f};   // 0..1 across the whole install
};
using InstallProgressFn = std::function<void(const InstallProgress&)>;

struct InstallOptions {
    std::string variant;               // provider variant id (empty = default)
    bool replace{false};               // reinstall over an existing copy of the same asset
    std::uint64_t max_total_bytes{2ull << 30};
    const std::atomic<bool>* cancel{nullptr};
    InstallProgressFn progress;
};

struct InstallResult {
    std::string folder;                // project-relative folder, e.g. "library/ambientcg/bricks113a"
    std::vector<std::string> files;    // project-relative (under assets/) paths written
    std::string primary;               // project-relative path to select or instantiate
    Uuid primary_uuid;                 // its asset UUID (nil when the database has no record)
    std::string material;              // the generated .vmat, when one was made
    bool already_installed{false};     // nothing was fetched: the same asset is in the project
};

// Installs `asset` into the project behind `db`: stage() then commit().
[[nodiscard]] Result<InstallResult> install(Provider& provider, HttpClient& http, const LibraryAsset& asset,
                                            assets::AssetDatabase& db, const InstallOptions& options = {});

// Imports a pack archive the user downloaded from the provider's own site (Kenney): extracts
// models (one format per pack, preferring GLB), textures, sprites and sounds under
// assets/library/<provider>/<slug>/ with the same licence records as install().
[[nodiscard]] Result<InstallResult> import_archive(const LibraryAsset& asset, const fs::Path& archive,
                                                   assets::AssetDatabase& db, const InstallOptions& options = {});

// The two halves of an install, so Studio can download on a worker thread and touch the
// AssetDatabase only on the main thread. stage() never reads or writes anything under assets/.
struct StagedInstall {
    LibraryAsset asset;
    std::string variant;
    fs::Path staging;                // <project>/.victus/library_staging/<key>
    std::string folder;              // destination under assets/
    std::vector<std::string> files;  // staged files, relative to the asset folder
    bool make_material{false};
    MaterialRecipe material;         // post-processed: one ORM path in occlusion/roughness/metallic
    bool alpha_masked{false};        // base colour carries a cut-out alpha (merged opacity)
    std::string primary;             // relative to the asset folder
    std::vector<std::string> source_urls;
    bool already_installed{false};
};
[[nodiscard]] Result<StagedInstall> stage(Provider& provider, HttpClient& http, const LibraryAsset& asset,
                                          const fs::Path& project_root, const InstallOptions& options = {});
[[nodiscard]] Result<StagedInstall> stage_archive(const LibraryAsset& asset, const fs::Path& archive,
                                                  const fs::Path& project_root, const InstallOptions& options = {});
// Moves the staged folder into assets/, refreshes the database, sets colour-space import
// settings, writes the .vmat and CREDITS.md. Consumes the staging folder on success and failure.
[[nodiscard]] Result<InstallResult> commit(StagedInstall staged, assets::AssetDatabase& db);
// Deletes a staging folder (cancelled installs).
void discard(const StagedInstall& staged);

// Provenance of one installed asset (assets/library/<provider>/<slug>/provenance.json).
struct InstalledAsset {
    LibraryAsset asset;
    std::string folder;        // project-relative under assets/
    std::string variant;
    std::string installed_at;  // ISO-8601 UTC
    std::vector<std::string> files;
};
// Every installed asset of a project, sorted by folder. Unreadable records are reported in
// `errors` and skipped.
[[nodiscard]] std::vector<InstalledAsset> installed_assets(const fs::Path& project_root, std::vector<std::string>* errors = nullptr);
// Folder an asset installs into ("library/<provider>/<slug>").
[[nodiscard]] std::string install_folder(const LibraryAsset& asset);
// Rewrites <project>/CREDITS.md from the provenance records (also called by install()).
[[nodiscard]] Status write_credits(const fs::Path& project_root);
// Removes an installed asset's folder (only folders that hold a provenance record).
[[nodiscard]] Status uninstall(assets::AssetDatabase& db, std::string_view folder);

// Raw deflate (RFC 1951) for zip::ReadOptions, backed by stb_image's zlib decoder.
[[nodiscard]] Result<std::vector<std::uint8_t>> inflate_raw(std::span<const std::uint8_t> compressed, std::size_t expected_size);

// Packs single-channel maps into an ORM PNG (R occlusion, G roughness, B metallic). Missing maps
// use the given constants. All present maps must share one size. Returns the PNG bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> pack_orm(const fs::Path& occlusion, const fs::Path& roughness,
                                                         const fs::Path& metallic, float default_roughness = 0.5f);
// Base colour (RGB) + greyscale opacity -> RGBA PNG bytes (decals, atlases, foliage cards).
[[nodiscard]] Result<std::vector<std::uint8_t>> merge_opacity(const fs::Path& base_color, const fs::Path& opacity);
// MD5 of `bytes` as lower-case hex (RFC 1321): integrity check against provider-published sums,
// not a security measure.
[[nodiscard]] std::string md5_hex(std::span<const std::uint8_t> bytes);
// Rewrites a DirectX-convention normal map as OpenGL convention (green inverted), PNG bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> flip_normal_green(const fs::Path& normal);

} // namespace victus::library
