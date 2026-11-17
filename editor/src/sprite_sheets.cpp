// SPDX-License-Identifier: Apache-2.0
// Sprite sheets in the editor: creating .vsheet assets from textures and assigning them.
#include "victus/editor/editor.hpp"

#include "editor_client.hpp"

#include "victus/animation/animation.hpp"

#include <filesystem>
#include <format>

namespace victus::editor {

using namespace scene;
namespace sfs = std::filesystem;

Result<Uuid> Editor::create_sprite_sheet(const Uuid& texture, std::uint32_t columns, std::uint32_t rows, float fps,
                                         std::string_view relative_path) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* t = db_->find(texture);
    if (!t || t->kind != assets::AssetKind::Texture)
        return make_error(ErrorCode::NotFound, "texture asset " + texture.to_string() + " not found");
    if (columns < 1 || rows < 1 || static_cast<std::uint64_t>(columns) * rows > animation::kMaxSpriteSheetFrames)
        return make_error(ErrorCode::InvalidArgument, std::format("a sheet needs 1..{} frames", animation::kMaxSpriteSheetFrames));
    if (!(fps > 0) || fps > 1000) return make_error(ErrorCode::InvalidArgument, "fps must be in (0, 1000]");
    const std::string ext(animation::kSpriteSheetExtension);
    std::string rel(relative_path);
    if (rel.empty()) {
        const fs::Path src(t->path);
        const std::string dir = src.parent_path().empty() ? std::string() : src.parent_path().generic_string() + "/";
        const std::string stem = src.stem().string();
        rel = dir + stem + ext;
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i) rel = std::format("{}{}_{}{}", dir, stem, i, ext);
    } else if (!rel.ends_with(ext)) {
        rel += ext;
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    if (sfs::exists(db_->assets_dir() / *clean)) return make_error(ErrorCode::AlreadyExists, *clean + " already exists");
    animation::SpriteSheet sheet;
    sheet.texture = texture;
    sheet.columns = columns;
    sheet.rows = rows;
    animation::SpriteClip clip;
    clip.name = "default";
    clip.fps = fps;
    for (std::uint32_t i = 0; i < columns * rows; ++i) clip.frames.push_back(i);
    sheet.clips.push_back(std::move(clip));
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    VICTUS_TRY_STATUS(animation::save_sprite_sheet_file(db_->assets_dir() / *clean, sheet));
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "sprite sheet was written but not registered: " + *clean);
    const Uuid id = rec->uuid;
    notify("Created sprite sheet " + *clean);
    return id;
}

bool Editor::assign_sprite_sheet(Entity e, const Uuid& sheet) {
    if (!db_ || e.is_null()) return false;
    auto data = db_->load_sprite_sheet(sheet);
    if (!data) {
        notify(data.error().message, true);
        return false;
    }
    Scene& s = active_scene();
    auto before = capture(s, {s.uuid(e)});
    auto& w = s.world();
    if (!w.has<SpriteComponent>(e)) w.add<SpriteComponent>(e);
    if (!w.has<animation::SpriteAnimatorComponent>(e)) w.add<animation::SpriteAnimatorComponent>(e);
    auto* a = w.get<animation::SpriteAnimatorComponent>(e);
    a->sheet = sheet;
    a->clip = data->clips.empty() ? std::string() : data->clips.front().name;
    a->time = 0;
    a->frame = 0;
    a->playing = true;
    auto* sp = w.get<SpriteComponent>(e);
    if (!data->texture.is_nil()) sp->texture_asset = data->texture;
    sp->uv = data->frame_uv(data->clips.empty() ? 0 : data->clips.front().frames.front());
    record_edit("Assign sprite sheet", std::move(before));
    return true;
}

} // namespace victus::editor
