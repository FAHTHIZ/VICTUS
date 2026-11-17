// SPDX-License-Identifier: Apache-2.0
// Terrain authoring (ADR-0063): creating terrain assets and entities.
#include "victus/editor/editor.hpp"

#include "victus/core/fs.hpp"
#include "victus/terrain/terrain.hpp"

#include <filesystem>
#include <format>

namespace victus::editor {

using namespace scene;
namespace sfs = std::filesystem;

Result<Entity> Editor::create_terrain(std::string_view relative_path, std::uint32_t resolution) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop play mode to create terrain");
    const std::string ext(terrain::kTerrainExtension);
    std::string rel(relative_path);
    if (rel.empty()) {
        rel = "terrain/Terrain" + ext;
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i) rel = std::format("terrain/Terrain_{}{}", i, ext);
    } else if (!rel.ends_with(ext)) {
        rel += ext;
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    if (sfs::exists(db_->assets_dir() / *clean)) return make_error(ErrorCode::AlreadyExists, *clean + " already exists");

    terrain::TerrainComponent comp;
    auto hf = terrain::make_flat(resolution);
    if (!hf) return std::move(hf).error();
    terrain::generate(*hf, {});
    terrain::auto_paint(*hf, comp.size);
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    VICTUS_TRY_STATUS(terrain::save_file(db_->assets_dir() / *clean, *hf));
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "terrain was written but not registered: " + *clean);
    comp.data = rec->uuid;

    Scene& s = *scene_;
    const Entity e = s.create("Terrain");
    auto& w = s.world();
    // Centre the terrain under the camera focus on the ground plane.
    w.get<Transform>(e)->position = {camera_.target.x, 0.0f, camera_.target.z};
    w.add<terrain::TerrainComponent>(e) = comp;
    s.update_transforms();
    if (terrains_) terrains_->update(s);
    const Uuid id = s.uuid(e);
    auto after = capture(s, {id});
    undo_.push({"Create Terrain", absent(after), std::move(after)});
    update_title();
    selection_ = {id};
    notify("Created terrain " + *clean);
    return e;
}

} // namespace victus::editor
