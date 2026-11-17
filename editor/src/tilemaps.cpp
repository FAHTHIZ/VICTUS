// SPDX-License-Identifier: Apache-2.0
// Tilemaps in the editor: creating .vtileset assets, the tile palette and viewport painting.
#include "victus/editor/editor.hpp"

#include "victus/runtime/scene_rendering.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/ui/ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>

namespace victus::editor {

using namespace scene;
namespace sfs = std::filesystem;

Result<Uuid> Editor::create_tileset(const Uuid& texture, std::uint32_t columns, std::uint32_t rows,
                                    tilemap::TileCollision collision, std::string_view relative_path) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* t = db_->find(texture);
    if (!t || t->kind != assets::AssetKind::Texture)
        return make_error(ErrorCode::NotFound, "texture asset " + texture.to_string() + " not found");
    if (columns < 1 || rows < 1 || static_cast<std::uint64_t>(columns) * rows > tilemap::kMaxTiles)
        return make_error(ErrorCode::InvalidArgument, std::format("a tile set needs 1..{} tiles", tilemap::kMaxTiles));
    const std::string ext(tilemap::kTilesetExtension);
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
    tilemap::Tileset ts;
    ts.texture = texture;
    ts.columns = columns;
    ts.rows = rows;
    ts.default_collision = collision;
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    VICTUS_TRY_STATUS(tilemap::save_tileset_file(db_->assets_dir() / *clean, ts));
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "tile set was written but not registered: " + *clean);
    const Uuid id = rec->uuid;
    if (tilemaps_) tilemaps_->clear_cache();
    notify("Created tile set " + *clean);
    return id;
}

bool Editor::assign_tileset(Entity e, const Uuid& tileset) {
    if (!db_) return false;
    const assets::AssetRecord* r = db_->find(tileset);
    if (!r || r->kind != assets::AssetKind::Tileset) {
        notify("tile set " + tileset.to_string() + " not found", true);
        return false;
    }
    if (e.is_null()) return false;
    Scene& s = active_scene();
    auto before = capture(s, {s.uuid(e)});
    auto& w = s.world();
    if (!w.has<tilemap::TilemapComponent>(e)) w.add<tilemap::TilemapComponent>(e);
    auto* tm = w.get<tilemap::TilemapComponent>(e);
    tm->tileset = tileset;
    ++tm->revision;
    record_edit("Assign tile set", std::move(before));
    tile_tool_ = true;
    show_tile_palette_ = true;
    return true;
}

Entity Editor::tile_target() {
    if (play_state_ != PlayState::Editing) return kNullEntity;
    const Entity e = selected();
    if (e.is_null() || !active_scene().world().has<tilemap::TilemapComponent>(e)) return kNullEntity;
    return e;
}

std::optional<std::array<std::int32_t, 2>> Editor::tile_under(Entity map, math::Vec2 pixel, math::Vec2 viewport_size) {
    Scene& s = active_scene();
    const auto* tm = map.is_null() ? nullptr : s.world().get<tilemap::TilemapComponent>(map);
    if (!tm || viewport_size.x <= 0 || viewport_size.y <= 0) return std::nullopt;
    if (runtime::scene_view_mode(s) != runtime::ViewMode::TwoD) return std::nullopt;
    const math::Vec2 world = camera_.camera2d(viewport_size.y).screen_to_world(pixel, viewport_size.x, viewport_size.y);
    const math::Vec3 local = math::inverse(s.world_matrix(map)).transform_point({world.x, world.y, 0});
    const double cx = std::floor(static_cast<double>(local.x) / std::max(tm->cell_size.x, 1e-6f));
    const double cy = std::floor(static_cast<double>(local.y) / std::max(tm->cell_size.y, 1e-6f));
    if (!(std::abs(cx) <= tilemap::kMaxCoordinate) || !(std::abs(cy) <= tilemap::kMaxCoordinate)) return std::nullopt;
    return std::array<std::int32_t, 2>{static_cast<std::int32_t>(cx), static_cast<std::int32_t>(cy)};
}

void Editor::apply_tile_brush(std::int32_t x, std::int32_t y) {
    Scene& s = active_scene();
    const Entity e = s.find(tile_stroke_.map);
    auto* tm = e.is_null() ? nullptr : s.world().get<tilemap::TilemapComponent>(e);
    if (!tm) return;
    switch (tile_brush_.mode) {
    case TileMode::Paint:
        if (const auto* c = tm->cell(x, y); c && c->tile == tile_brush_.tile && c->terrain < 0 &&
                                              c->flip_x == tile_brush_.flip_x && c->flip_y == tile_brush_.flip_y)
            return;
        (void)tilemap::set_tile(*tm, x, y, tile_brush_.tile, tile_brush_.flip_x, tile_brush_.flip_y);
        break;
    case TileMode::Erase: {
        const auto* c = tm->cell(x, y);
        if (!c) return;
        const auto ts = c->terrain >= 0 && tilemaps_ ? tilemaps_->tileset(tm->tileset) : nullptr;
        if (ts) (void)tilemap::paint_terrain(*tm, *ts, x, y, -1);
        else (void)tilemap::set_tile(*tm, x, y, -1);
        break;
    }
    case TileMode::Terrain: {
        const auto ts = tilemaps_ ? tilemaps_->tileset(tm->tileset) : nullptr;
        if (!ts) return;
        if (const auto* c = tm->cell(x, y); c && c->terrain == tile_brush_.terrain) return;
        (void)tilemap::paint_terrain(*tm, *ts, x, y, tile_brush_.terrain);
        break;
    }
    case TileMode::Rectangle:
    case TileMode::Pick: break;
    }
}

bool Editor::begin_tile_stroke(Entity map, std::int32_t x, std::int32_t y) {
    if (tile_stroke_.active) end_tile_stroke();
    Scene& s = active_scene();
    const auto* tm = map.is_null() ? nullptr : s.world().get<tilemap::TilemapComponent>(map);
    if (!tm) return false;
    if (tile_brush_.mode == TileMode::Pick) {
        if (const auto* c = tm->cell(x, y)) {
            tile_brush_.tile = c->tile;
            tile_brush_.flip_x = c->flip_x;
            tile_brush_.flip_y = c->flip_y;
            if (c->terrain >= 0) {
                tile_brush_.terrain = c->terrain;
                tile_brush_.mode = TileMode::Terrain;
            } else {
                tile_brush_.mode = TileMode::Paint;
            }
        }
        return true;
    }
    tile_stroke_ = {};
    tile_stroke_.active = true;
    tile_stroke_.map = s.uuid(map);
    tile_stroke_.x0 = tile_stroke_.x = x;
    tile_stroke_.y0 = tile_stroke_.y = y;
    tile_stroke_.before = capture(s, {tile_stroke_.map});
    tile_stroke_.revision = tm->revision;
    apply_tile_brush(x, y);
    return true;
}

void Editor::continue_tile_stroke(std::int32_t x, std::int32_t y) {
    if (!tile_stroke_.active) return;
    if (tile_brush_.mode == TileMode::Rectangle) {
        tile_stroke_.x = x;
        tile_stroke_.y = y;
        return;
    }
    // Bresenham from the last cell so fast drags leave no gaps.
    std::int32_t cx = tile_stroke_.x, cy = tile_stroke_.y;
    const std::int32_t dx = std::abs(x - cx), dy = -std::abs(y - cy);
    const std::int32_t sx = cx < x ? 1 : -1, sy = cy < y ? 1 : -1;
    std::int32_t err = dx + dy;
    for (int guard = 0; guard < 65536 && (cx != x || cy != y); ++guard) {
        const std::int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            cx += sx;
        }
        if (e2 <= dx) {
            err += dx;
            cy += sy;
        }
        apply_tile_brush(cx, cy);
    }
    tile_stroke_.x = x;
    tile_stroke_.y = y;
}

void Editor::end_tile_stroke() {
    if (!tile_stroke_.active) return;
    TileStroke stroke = std::move(tile_stroke_);
    tile_stroke_ = {};
    Scene& s = active_scene();
    const Entity e = s.find(stroke.map);
    auto* tm = e.is_null() ? nullptr : s.world().get<tilemap::TilemapComponent>(e);
    if (!tm) return;
    if (tile_brush_.mode == TileMode::Rectangle)
        (void)tilemap::fill_tiles(*tm, stroke.x0, stroke.y0, stroke.x, stroke.y, tile_brush_.tile);
    if (tm->revision == stroke.revision) return; // nothing changed: no undo entry
    record_edit(tile_brush_.mode == TileMode::Erase ? "Erase tiles" : "Paint tiles", std::move(stroke.before));
}

void Editor::tile_viewport_input(math::Vec2 origin, math::Vec2 size, bool hovered) {
    tile_hover_.reset();
    const Entity map = tile_tool_ ? tile_target() : kNullEntity;
    if (map.is_null() || runtime::scene_view_mode(active_scene()) != runtime::ViewMode::TwoD) {
        if (tile_stroke_.active) end_tile_stroke();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const auto cell = tile_under(map, {io.MousePos.x - origin.x, io.MousePos.y - origin.y}, size);
    if (hovered) tile_hover_ = cell;
    if (hovered && cell && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        (void)begin_tile_stroke(map, (*cell)[0], (*cell)[1]);
    else if (tile_stroke_.active && cell && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        continue_tile_stroke((*cell)[0], (*cell)[1]);
    if (tile_stroke_.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) end_tile_stroke();
    if (hovered && !io.WantTextInput && !io.KeyCtrl && !tile_stroke_.active) {
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) tile_brush_.mode = TileMode::Paint;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tile_brush_.mode = TileMode::Erase;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tile_brush_.mode = TileMode::Rectangle;
        if (ImGui::IsKeyPressed(ImGuiKey_T, false)) tile_brush_.mode = TileMode::Terrain;
        if (ImGui::IsKeyPressed(ImGuiKey_I, false)) tile_brush_.mode = TileMode::Pick;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) tile_brush_.flip_x = !tile_brush_.flip_x;
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) tile_brush_.flip_y = !tile_brush_.flip_y;
    }
}

void Editor::draw_tile_overlay(render::SpriteBatch& b, float px) {
    const Entity map = tile_tool_ ? tile_target() : kNullEntity;
    if (map.is_null()) return;
    Scene& s = active_scene();
    const auto* tm = s.world().get<tilemap::TilemapComponent>(map);
    if (!tm) return;
    const math::Mat4 m = s.world_matrix(map);
    const float cw = tm->cell_size.x, ch = tm->cell_size.y;
    auto line = [&](float x0, float y0, float x1, float y1, float width, const math::Color& c, std::int32_t layer) {
        const math::Vec3 a = m.transform_point({x0, y0, 0});
        const math::Vec3 d = m.transform_point({x1, y1, 0});
        b.draw_line({a.x, a.y}, {d.x, d.y}, width, c, layer);
    };
    auto rect = [&](std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, const math::Color& c) {
        const float ax = static_cast<float>(std::min(x0, x1)) * cw, ay = static_cast<float>(std::min(y0, y1)) * ch;
        const float bx = static_cast<float>(std::max(x0, x1) + 1) * cw, by = static_cast<float>(std::max(y0, y1) + 1) * ch;
        line(ax, ay, bx, ay, 2 * px, c, 1002);
        line(bx, ay, bx, by, 2 * px, c, 1002);
        line(bx, by, ax, by, 2 * px, c, 1002);
        line(ax, by, ax, ay, 2 * px, c, 1002);
    };
    // Grid around the painted area (plus a margin), capped so huge maps stay cheap.
    tilemap::TileBounds g = tilemap::bounds(*tm);
    if (g.empty()) g = {0, 0, 0, 0};
    if (tile_hover_) {
        g.min_x = std::min(g.min_x, (*tile_hover_)[0]);
        g.min_y = std::min(g.min_y, (*tile_hover_)[1]);
        g.max_x = std::max(g.max_x, (*tile_hover_)[0]);
        g.max_y = std::max(g.max_y, (*tile_hover_)[1]);
    }
    constexpr std::int32_t kMargin = 8, kMaxLines = 256;
    g.min_x -= kMargin;
    g.min_y -= kMargin;
    g.max_x = std::min(g.max_x + kMargin, g.min_x + kMaxLines);
    g.max_y = std::min(g.max_y + kMargin, g.min_y + kMaxLines);
    const math::Color grid{1, 1, 1, 0.12f};
    for (std::int32_t x = g.min_x; x <= g.max_x + 1; ++x)
        line(static_cast<float>(x) * cw, static_cast<float>(g.min_y) * ch, static_cast<float>(x) * cw,
             static_cast<float>(g.max_y + 1) * ch, px, grid, 1001);
    for (std::int32_t y = g.min_y; y <= g.max_y + 1; ++y)
        line(static_cast<float>(g.min_x) * cw, static_cast<float>(y) * ch, static_cast<float>(g.max_x + 1) * cw,
             static_cast<float>(y) * ch, px, grid, 1001);
    if (tile_stroke_.active && tile_brush_.mode == TileMode::Rectangle)
        rect(tile_stroke_.x0, tile_stroke_.y0, tile_stroke_.x, tile_stroke_.y, {1.0f, 0.55f, 0.1f, 1});
    else if (tile_hover_)
        rect((*tile_hover_)[0], (*tile_hover_)[1], (*tile_hover_)[0], (*tile_hover_)[1],
             tile_brush_.mode == TileMode::Erase ? math::Color{1, 0.3f, 0.3f, 1} : math::Color{1, 1, 1, 0.9f});
}

void Editor::draw_tile_palette() {
    if (!show_tile_palette_) return;
    if (!ImGui::Begin("Tile Palette", &show_tile_palette_)) {
        ImGui::End();
        return;
    }
    const Entity map = tile_target();
    const auto* tm = map.is_null() ? nullptr : active_scene().world().get<tilemap::TilemapComponent>(map);
    if (!tm) {
        ImGui::TextDisabled("Select an entity with a Tilemap component,");
        ImGui::TextDisabled("or double-click a tile set in Assets.");
        ImGui::End();
        return;
    }
    ImGui::Checkbox("Paint in viewport", &tile_tool_);
    if (runtime::scene_view_mode(active_scene()) != runtime::ViewMode::TwoD)
        ImGui::TextDisabled("Painting works in 2D scenes.");
    struct ModeLabel {
        TileMode mode;
        const char* label;
        const char* key;
    };
    static constexpr ModeLabel kModes[] = {{TileMode::Paint, "Paint", "B"},      {TileMode::Erase, "Erase", "E"},
                                           {TileMode::Rectangle, "Rect", "R"},   {TileMode::Terrain, "Terrain", "T"},
                                           {TileMode::Pick, "Pick", "I"}};
    for (const auto& m : kModes) {
        if (&m != kModes) ImGui::SameLine();
        if (ImGui::RadioButton(m.label, tile_brush_.mode == m.mode)) tile_brush_.mode = m.mode;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shortcut: %s (viewport hovered)", m.key);
    }
    ImGui::Checkbox("Flip X", &tile_brush_.flip_x);
    ImGui::SameLine();
    ImGui::Checkbox("Flip Y", &tile_brush_.flip_y);
    const auto ts = tilemaps_ ? tilemaps_->tileset(tm->tileset) : nullptr;
    if (!ts) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Tile set not assigned or not loaded.");
        ImGui::End();
        return;
    }
    if (!ts->terrains.empty()) {
        ImGui::SeparatorText("Terrains");
        for (std::size_t i = 0; i < ts->terrains.size(); ++i) {
            const bool on = tile_brush_.mode == TileMode::Terrain && tile_brush_.terrain == static_cast<std::int32_t>(i);
            if (ImGui::Selectable(std::format("{}##terrain{}", ts->terrains[i].name, i).c_str(), on)) {
                tile_brush_.terrain = static_cast<std::int32_t>(i);
                tile_brush_.mode = TileMode::Terrain;
            }
        }
    }
    ImGui::SeparatorText(std::format("Tiles ({})", ts->tile_count()).c_str());
    rhi::TextureHandle tex{};
    if (cache_ && !ts->texture.is_nil())
        if (auto t = cache_->texture(ts->texture)) tex = *t;
    const float cell = 32.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImGuiStyle& style = ImGui::GetStyle();
    const int per_row = std::max(1, static_cast<int>(avail / (cell + 2 * style.FramePadding.x + style.ItemSpacing.x)));
    const std::uint32_t shown = std::min<std::uint32_t>(ts->tile_count(), 4096);
    for (std::uint32_t i = 0; i < shown; ++i) {
        if (i % static_cast<std::uint32_t>(per_row) != 0) ImGui::SameLine();
        const bool on = tile_brush_.tile == static_cast<std::int32_t>(i) &&
                        (tile_brush_.mode == TileMode::Paint || tile_brush_.mode == TileMode::Rectangle);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.55f, 0.1f, 1.0f));
        bool clicked = false;
        const std::string id = std::format("##tile{}", i);
        if (tex.valid()) {
            const math::Vec4 uv = ts->tile_uv(i);
            clicked = ImGui::ImageButton(id.c_str(), ImTextureRef(ui::UiContext::texture_id(tex)), ImVec2(cell, cell),
                                         ImVec2(uv.x, uv.y), ImVec2(uv.z, uv.w));
        } else {
            clicked = ImGui::Button(std::format("{}{}", i, id).c_str(), ImVec2(cell + 2 * style.FramePadding.x,
                                                                             cell + 2 * style.FramePadding.y));
        }
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            const char* col = tilemap::to_string(ts->collision_of(static_cast<std::int32_t>(i)));
            ImGui::SetTooltip("Tile %u (collision: %s)", i, col);
        }
        if (clicked) {
            tile_brush_.tile = static_cast<std::int32_t>(i);
            if (tile_brush_.mode != TileMode::Rectangle) tile_brush_.mode = TileMode::Paint;
            tile_tool_ = true;
        }
    }
    ImGui::End();
}

} // namespace victus::editor
