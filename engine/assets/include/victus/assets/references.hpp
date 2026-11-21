// SPDX-License-Identifier: Apache-2.0
// Asset references (ADR-0175): which project files use an asset and which assets a file uses,
// plus reference-safe rename/move and delete.
//
// VICTUS files refer to assets in two ways: by stable UUID (models, materials, textures, sheets
// in scenes, prefabs and .vmat files) and by assets-relative path (scripts, sounds, Luau string
// literals). The index reads every text file of the project (scenes/, assets/ and victus.project)
// and records both kinds, so "Used by" in Studio matches what actually breaks when the asset goes
// away. Binary files (images, models, audio, cooked data) are not scanned: they cannot name
// another project asset by UUID or project path.
#pragma once
#include "victus/assets/database.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::assets {

struct AssetReference {
    std::string file; // project-relative, '/' separated ("scenes/main.vscene", "assets/x.vmat")
    Uuid target;      // the referenced asset
    bool by_path{false}; // named by its assets-relative path (breaks on rename/move)
    std::uint32_t count{1}; // occurrences in the file
};

class ReferenceIndex {
public:
    // Scans the project of `db` (scenes/, assets/, victus.project). Files larger than 16 MiB and
    // files with NUL bytes are skipped.
    [[nodiscard]] static ReferenceIndex build(const AssetDatabase& db);

    // Files that refer to `asset`, sorted by file. An asset never lists itself.
    [[nodiscard]] std::vector<AssetReference> users(const Uuid& asset) const;
    // Assets referred to by `file` (project-relative), sorted by target path.
    [[nodiscard]] std::vector<AssetReference> dependencies(std::string_view file) const;
    // Assets no scanned file refers to (by UUID or path); scenes are never listed.
    [[nodiscard]] std::vector<Uuid> unreferenced(const AssetDatabase& db) const;
    [[nodiscard]] std::size_t files_scanned() const noexcept { return files_scanned_; }
    [[nodiscard]] const std::vector<AssetReference>& all() const noexcept { return refs_; }

private:
    std::vector<AssetReference> refs_;
    std::size_t files_scanned_{0};
};

// Project-relative path of an asset source ("assets/" + record path).
[[nodiscard]] std::string project_relative(const AssetRecord& r);

struct RenameReport {
    std::string from, to;              // assets-relative
    std::vector<std::string> rewritten; // project files whose path references were updated
};

// Moves an asset (and its .meta) to `new_relative` under assets/, keeping its UUID, then
// rewrites every path reference to it in project text files and refreshes the database.
// Fails without changing anything when the target exists, the name is unsafe, or the asset has
// companion files that must stay beside it (a .gltf moved to another folder).
[[nodiscard]] Result<RenameReport> rename_asset(AssetDatabase& db, const Uuid& asset, std::string_view new_relative);

// Deletes an asset's source, .meta and cooked data, then refreshes the database. References to
// it are left as they are (callers show ReferenceIndex::users first).
[[nodiscard]] Status delete_asset(AssetDatabase& db, const Uuid& asset);

// ---- trash (ADR-0177) ------------------------------------------------------------------------
// Recoverable delete: the source and its .meta move to <project>/.victus/trash/<id>/ with an
// entry.json recording the original path, so Restore brings the asset back with the same UUID
// (every by-ID reference works again). Cooked data is dropped and rebuilt on restore. The trash
// is local to this machine (.victus/ is not committed); deleting .victus/ empties it.
struct TrashEntry {
    std::string id;            // folder name under .victus/trash
    std::string original_path; // assets-relative path the asset had
    Uuid uuid;
    std::int64_t deleted_unix{0};
    std::uint64_t bytes{0};    // size of the trashed source
};
[[nodiscard]] fs::Path trash_dir(const AssetDatabase& db);
[[nodiscard]] Result<TrashEntry> trash_asset(AssetDatabase& db, const Uuid& asset);
// Newest first. Folders without a readable entry.json are skipped.
[[nodiscard]] std::vector<TrashEntry> list_trash(const AssetDatabase& db);
// Moves the asset back to its original path. Fails when that path is taken or an asset with the
// same UUID exists again; `to` restores under a different assets-relative path instead.
[[nodiscard]] Status restore_from_trash(AssetDatabase& db, std::string_view id, std::string_view to = {});
// Removes one entry for good (all entries when `id` is empty).
[[nodiscard]] Status purge_trash(AssetDatabase& db, std::string_view id = {});

} // namespace victus::assets
