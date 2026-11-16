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
#include "victus/assets/importers.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/jobs.hpp"

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

class AssetDatabase {
public:
    // Opens (and creates assets/ and .victus/cache/ if missing) and runs refresh().
    [[nodiscard]] static Result<std::unique_ptr<AssetDatabase>> open(const fs::Path& project_root,
                                                                     RefreshReport* report = nullptr);

    // Rescans assets/, creating .meta files for new sources.
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

    // Changes import settings (writes the .meta); the next import re-cooks.
    [[nodiscard]] Status set_settings(const Uuid& uuid, json::Value settings);

    [[nodiscard]] const fs::Path& project_root() const noexcept { return root_; }
    [[nodiscard]] fs::Path assets_dir() const { return root_ / "assets"; }
    [[nodiscard]] fs::Path cache_dir() const { return root_ / ".victus" / "cache"; }
    [[nodiscard]] fs::Path source_path(const AssetRecord& r) const;
    [[nodiscard]] fs::Path cooked_path(const Uuid& uuid) const;

private:
    explicit AssetDatabase(fs::Path root) : root_(std::move(root)) {}
    [[nodiscard]] Result<std::uint64_t> source_key(const AssetRecord& r) const;
    [[nodiscard]] Status write_meta(const AssetRecord& r) const;

    fs::Path root_;
    mutable std::mutex mutex_;
    std::vector<AssetRecord> records_;
    std::unordered_map<Uuid, std::size_t, UuidHash> by_uuid_;
    std::unordered_map<std::string, std::size_t> by_path_;
};

} // namespace victus::assets
