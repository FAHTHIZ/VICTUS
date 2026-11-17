// SPDX-License-Identifier: Apache-2.0
// Terrain authoring (ADR-0063): creating terrain assets and entities, sculpt and paint brushes
// (one undo entry per stroke holding the changed samples before and after), whole-terrain
// generate / auto paint, and writing edited heightfields back on save.
#include "victus/editor/editor.hpp"

#include "victus/core/fs.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/terrain/terrain.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>

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

Entity Editor::terrain_target() {
    if (play_state_ != PlayState::Editing) return kNullEntity;
    const Entity e = selected();
    if (e.is_null() || !active_scene().world().has<terrain::TerrainComponent>(e)) return kNullEntity;
    return e;
}

std::shared_ptr<terrain::Heightfield> Editor::editable_terrain(Entity t, Uuid* id) {
    Scene& s = active_scene();
    const auto* comp = t.is_null() ? nullptr : s.world().get<terrain::TerrainComponent>(t);
    if (!comp || !terrains_) return nullptr;
    auto data = terrains_->data(comp->data);
    if (!data || data->empty()) return nullptr;
    terrains_->add_data(comp->data, data); // survives clear_cache() on asset reloads
    if (id) *id = comp->data;
    return data;
}

std::optional<terrain::TerrainHit> Editor::terrain_under(math::Vec2 pixel, math::Vec2 viewport_size) {
    if (viewport_size.x <= 0 || viewport_size.y <= 0) return std::nullopt;
    Scene& s = active_scene();
    if (runtime::scene_view_mode(s) == runtime::ViewMode::TwoD) return std::nullopt;
    if (terrains_) terrains_->update(s);
    const gizmo::View::Ray ray = gizmo_view(viewport_size).ray(pixel);
    return terrain::raycast(s, ray.origin, ray.direction, 1e5f);
}

bool Editor::begin_terrain_stroke(Entity t, math::Vec3 world_point, bool invert) {
    if (terrain_stroke_.active) end_terrain_stroke();
    if (play_state_ != PlayState::Editing) return false;
    Scene& s = active_scene();
    Uuid id;
    const auto data = editable_terrain(t, &id);
    const auto* wt = data ? s.world().get<WorldTransform>(t) : nullptr;
    if (!wt) return false;
    terrain_stroke_ = {};
    terrain_stroke_.active = true;
    terrain_stroke_.data = id;
    terrain_stroke_.brush = terrain_brush_;
    if (invert && terrain_brush_.mode == terrain::BrushMode::Raise) terrain_stroke_.brush.mode = terrain::BrushMode::Lower;
    else if (invert && terrain_brush_.mode == terrain::BrushMode::Lower) terrain_stroke_.brush.mode = terrain::BrushMode::Raise;
    terrain_stroke_.to_local = math::inverse(terrain::rigid_matrix(wt->matrix));
    terrain_stroke_.last = terrain_stroke_.to_local.transform_point(world_point);
    terrain_stroke_.original.resolution = data->resolution;
    terrain_stroke_.original.heights = data->heights;
    terrain_stroke_.original.splat = data->splat;
    return true;
}

void Editor::continue_terrain_stroke(math::Vec3 world_point, float amount) {
    TerrainStroke& st = terrain_stroke_;
    if (!st.active || !terrains_) return;
    const auto data = terrains_->data(st.data);
    if (!data || data->resolution != st.original.resolution) return;
    const Entity t = terrain_target();
    const auto* comp = t.is_null() ? nullptr : active_scene().world().get<terrain::TerrainComponent>(t);
    if (!comp || comp->data != st.data) return;
    const math::Vec3 p = st.to_local.transform_point(world_point);
    // Dabs at most a quarter radius apart so fast drags leave no gaps; the amount is shared.
    const float dx = p.x - st.last.x, dz = p.z - st.last.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    const float spacing = std::max(st.brush.radius * 0.25f, 1e-3f);
    const int steps = std::clamp(static_cast<int>(std::ceil(dist / spacing)), 1, 256);
    for (int i = 1; i <= steps; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(steps);
        st.changed.merge(terrain::apply_brush(*data, comp->size, st.brush, st.last.x + dx * f, st.last.z + dz * f,
                                              amount / static_cast<float>(steps)));
    }
    st.last = p;
}

void Editor::push_terrain_edit(std::string label, const Uuid& data, terrain::HeightfieldRegion before,
                               terrain::HeightfieldRegion after) {
    dirty_terrains_.insert(data);
    if (play_state_ != PlayState::Editing) return;
    auto regions = std::make_shared<const std::pair<terrain::HeightfieldRegion, terrain::HeightfieldRegion>>(
        std::move(before), std::move(after));
    SceneCommand c;
    c.label = std::move(label);
    c.external = [this, data, regions](bool redo) -> Status {
        const auto hf = terrains_ ? terrains_->data(data) : nullptr;
        if (!hf) return make_error(ErrorCode::NotFound, "terrain data " + data.to_string() + " is not loaded");
        terrain::paste_region(*hf, redo ? regions->second : regions->first);
        dirty_terrains_.insert(data);
        return {};
    };
    undo_.push(std::move(c));
    update_title();
}

void Editor::end_terrain_stroke() {
    if (!terrain_stroke_.active) return;
    TerrainStroke st = std::move(terrain_stroke_);
    terrain_stroke_ = {};
    if (st.changed.empty() || !terrains_) return; // nothing changed: no undo entry
    const auto data = terrains_->data(st.data);
    if (!data) return;
    push_terrain_edit(st.brush.mode == terrain::BrushMode::Paint ? "Paint terrain" : "Sculpt terrain", st.data,
                      terrain::copy_region(st.original, st.changed), terrain::copy_region(*data, st.changed));
}

Status Editor::generate_terrain(Entity t, const terrain::GenerateSettings& settings) {
    Uuid id;
    const auto data = editable_terrain(t, &id);
    if (!data) return make_error(ErrorCode::InvalidArgument, "entity has no loaded terrain");
    const terrain::SampleRect all{0, 0, static_cast<std::int32_t>(data->resolution) - 1,
                                  static_cast<std::int32_t>(data->resolution) - 1};
    auto before = terrain::copy_region(*data, all);
    terrain::generate(*data, settings);
    data->touch_all();
    push_terrain_edit("Generate terrain", id, std::move(before), terrain::copy_region(*data, all));
    return {};
}

Status Editor::auto_paint_terrain(Entity t, const terrain::AutoPaintSettings& settings) {
    Uuid id;
    const auto data = editable_terrain(t, &id);
    if (!data) return make_error(ErrorCode::InvalidArgument, "entity has no loaded terrain");
    const terrain::SampleRect all{0, 0, static_cast<std::int32_t>(data->resolution) - 1,
                                  static_cast<std::int32_t>(data->resolution) - 1};
    auto before = terrain::copy_region(*data, all);
    terrain::auto_paint(*data, active_scene().world().get<terrain::TerrainComponent>(t)->size, settings);
    data->touch_all();
    push_terrain_edit("Auto paint terrain", id, std::move(before), terrain::copy_region(*data, all));
    return {};
}

Status Editor::save_terrains() {
    if (!db_ || !terrains_) return {};
    for (auto it = dirty_terrains_.begin(); it != dirty_terrains_.end();) {
        const assets::AssetRecord* rec = db_->find(*it);
        const auto data = terrains_->data(*it);
        if (!rec || !data) {
            it = dirty_terrains_.erase(it); // the asset is gone: nothing to write
            continue;
        }
        VICTUS_TRY_STATUS(terrain::save_file(db_->assets_dir() / rec->path, *data));
        it = dirty_terrains_.erase(it);
    }
    return {};
}

void Editor::terrain_viewport_input(math::Vec2 origin, math::Vec2 size, bool hovered) {
    terrain_hover_.reset();
    const Entity t = terrain_tool_ ? terrain_target() : kNullEntity;
    if (t.is_null() || runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD) {
        if (terrain_stroke_.active) end_terrain_stroke();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    std::optional<terrain::TerrainHit> hit;
    if (hovered || terrain_stroke_.active) {
        hit = terrain_under({io.MousePos.x - origin.x, io.MousePos.y - origin.y}, size);
        if (hit && hit->entity != t) hit.reset();
    }
    if (hovered) terrain_hover_ = hit;
    if (hovered && hit && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyCtrl && terrain_brush_.mode == terrain::BrushMode::Flatten) {
            const auto* comp = active_scene().world().get<terrain::TerrainComponent>(t);
            if (comp && comp->size.y > 0) terrain_brush_.target = std::clamp(hit->local.y / comp->size.y, 0.0f, 1.0f);
        } else if (begin_terrain_stroke(t, hit->point, io.KeyShift)) {
            continue_terrain_stroke(hit->point, io.DeltaTime);
        }
    } else if (terrain_stroke_.active && hit && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        continue_terrain_stroke(hit->point, io.DeltaTime);
    }
    if (terrain_stroke_.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) end_terrain_stroke();
    if (hovered && !io.WantTextInput && !io.KeyCtrl && !terrain_stroke_.active) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) terrain_brush_.mode = terrain::BrushMode::Raise;
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) terrain_brush_.mode = terrain::BrushMode::Lower;
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) terrain_brush_.mode = terrain::BrushMode::Smooth;
        if (ImGui::IsKeyPressed(ImGuiKey_4, false)) terrain_brush_.mode = terrain::BrushMode::Flatten;
        if (ImGui::IsKeyPressed(ImGuiKey_5, false)) terrain_brush_.mode = terrain::BrushMode::Paint;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) terrain_brush_.radius = std::max(terrain_brush_.radius / 1.15f, 0.25f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) terrain_brush_.radius = std::min(terrain_brush_.radius * 1.15f, 512.0f);
    }
}

void Editor::draw_terrain_overlay(render::SceneRenderer& r) {
    if (!terrain_hover_ || !terrain_tool_) return;
    const Entity t = terrain_target();
    if (t.is_null() || t != terrain_hover_->entity) return;
    Scene& s = active_scene();
    const auto* comp = s.world().get<terrain::TerrainComponent>(t);
    const auto* wt = s.world().get<WorldTransform>(t);
    if (!comp || !wt) return;
    const math::Mat4 m = terrain::rigid_matrix(wt->matrix);
    const math::Vec3 c = terrain_hover_->local;
    // Outer radius and the inner full-strength ring, draped over the surface.
    auto ring = [&](float radius, math::Color color) {
        constexpr int kSegments = 48;
        std::optional<math::Vec3> prev;
        for (int i = 0; i <= kSegments; ++i) {
            const float a = static_cast<float>(i) / kSegments * 2.0f * std::numbers::pi_v<float>;
            const float x = c.x + std::cos(a) * radius, z = c.z + std::sin(a) * radius;
            const math::Vec3 world = m.transform_point({x, c.y, z});
            const auto h = terrain::height_at(s, t, world.x, world.z);
            const math::Vec3 p = h ? math::Vec3{world.x, *h + 0.05f, world.z} : world;
            if (prev) r.line(*prev, p, color);
            prev = p;
        }
    };
    const math::Color color = terrain_brush_.mode == terrain::BrushMode::Paint ? math::Color{0.3f, 0.8f, 1.0f, 1}
                                                                               : math::Color{1.0f, 0.75f, 0.2f, 1};
    ring(terrain_brush_.radius, color);
    if (terrain_brush_.falloff > 0.01f && terrain_brush_.falloff < 0.99f)
        ring(terrain_brush_.radius * (1.0f - terrain_brush_.falloff), {color.r * 0.6f, color.g * 0.6f, color.b * 0.6f, 1});
    r.line(terrain_hover_->point, terrain_hover_->point + terrain_hover_->normal * std::max(terrain_brush_.radius * 0.3f, 0.5f),
           color);
}

void Editor::draw_terrain_tools() {
    if (!show_terrain_tools_) return;
    const Entity t = terrain_target();
    if (t.is_null()) return; // only shown while a terrain is selected
    if (!ImGui::Begin("Terrain Tools", &show_terrain_tools_)) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("Sculpt in viewport", &terrain_tool_);
    if (runtime::scene_view_mode(active_scene()) == runtime::ViewMode::TwoD)
        ImGui::TextDisabled("Sculpting works in 3D scenes.");
    static constexpr terrain::BrushMode kModes[] = {terrain::BrushMode::Raise, terrain::BrushMode::Lower,
                                                    terrain::BrushMode::Smooth, terrain::BrushMode::Flatten,
                                                    terrain::BrushMode::Paint};
    for (std::size_t i = 0; i < std::size(kModes); ++i) {
        if (i) ImGui::SameLine();
        const std::string label(terrain::brush_mode_name(kModes[i]));
        if (ImGui::RadioButton(std::format("{} ({})", label, i + 1).c_str(), terrain_brush_.mode == kModes[i])) {
            terrain_brush_.mode = kModes[i];
            terrain_tool_ = true;
        }
    }
    ImGui::DragFloat("Radius (m)", &terrain_brush_.radius, 0.1f, 0.25f, 512.0f, "%.2f");
    ImGui::SliderFloat("Strength", &terrain_brush_.strength, 0.0f, 1.0f);
    ImGui::SliderFloat("Falloff", &terrain_brush_.falloff, 0.0f, 1.0f);
    if (terrain_brush_.mode == terrain::BrushMode::Flatten) {
        ImGui::SliderFloat("Target height", &terrain_brush_.target, 0.0f, 1.0f);
        ImGui::TextDisabled("Ctrl-click the terrain to pick the target.");
    }
    if (terrain_brush_.mode == terrain::BrushMode::Paint) {
        static constexpr const char* kLayers[] = {"Layer 0 (grass)", "Layer 1 (rock)", "Layer 2 (sand)", "Layer 3 (snow)"};
        int layer = static_cast<int>(std::min(terrain_brush_.layer, terrain::kLayerCount - 1));
        if (ImGui::Combo("Layer", &layer, kLayers, static_cast<int>(std::size(kLayers))))
            terrain_brush_.layer = static_cast<std::uint32_t>(layer);
    }
    ImGui::TextDisabled("Shift inverts raise/lower, [ and ] resize, 1-5 pick a mode.");
    ImGui::Separator();
    int seed = static_cast<int>(terrain_generate_.seed & 0x7fffffff);
    if (ImGui::InputInt("Seed", &seed)) terrain_generate_.seed = static_cast<std::uint64_t>(std::max(seed, 0));
    ImGui::SliderFloat("Frequency", &terrain_generate_.frequency, 0.5f, 16.0f);
    ImGui::SliderFloat("Amplitude", &terrain_generate_.amplitude, 0.0f, 1.0f);
    ImGui::SliderFloat("Ridges", &terrain_generate_.ridge, 0.0f, 1.0f);
    if (ImGui::Button("Generate"))
        if (auto st = generate_terrain(t, terrain_generate_); !st) notify("generate failed: " + st.error().message, true);
    ImGui::SameLine();
    if (ImGui::Button("Auto Paint"))
        if (auto st = auto_paint_terrain(t); !st) notify("auto paint failed: " + st.error().message, true);
    if (!dirty_terrains_.empty()) ImGui::TextDisabled("%zu edited terrain(s) are written on save.", dirty_terrains_.size());
    ImGui::End();
}

} // namespace victus::editor
