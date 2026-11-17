// SPDX-License-Identifier: Apache-2.0
// Material assets in the editor: creating .vmat files, assigning them to mesh renderers (undoable)
// and the Material Editor window, which edits one material file with live viewport feedback.
// Material files are project assets, not scene data: their edits are saved to disk directly and are
// not part of scene undo (like prefab files).
#include "victus/editor/editor.hpp"

#include "editor_client.hpp"

#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/surface_shader.hpp"

#include <imgui.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>

namespace victus::editor {

using namespace scene;
namespace sfs = std::filesystem;

namespace {
constexpr const char* kAssetPayload = "VICTUS_ASSET"; // matches the asset browser's drag source

std::string clean_base(std::string_view name, std::string_view fallback) {
    std::string base(name);
    for (char& ch : base)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
    return base.empty() ? std::string(fallback) : base;
}
} // namespace

Result<Uuid> Editor::create_material(std::string_view relative_path, const assets::MaterialAsset& material) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const std::string ext(assets::kMaterialExtension);
    std::string rel(relative_path);
    if (rel.empty()) {
        rel = "materials/Material" + ext;
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i) rel = std::format("materials/Material_{}{}", i, ext);
    } else if (!rel.ends_with(ext)) {
        rel += ext;
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    if (sfs::exists(db_->assets_dir() / *clean)) return make_error(ErrorCode::AlreadyExists, *clean + " already exists");
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    VICTUS_TRY_STATUS(assets::save_material_file(db_->assets_dir() / *clean, material));
    if (auto text = fs::read_text(db_->assets_dir() / *clean); text) written_materials_[*clean] = fnv1a64(*text);
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "material was written but not registered: " + *clean);
    notify("Created material " + *clean);
    return rec->uuid;
}

Status Editor::save_material(const Uuid& asset, const assets::MaterialAsset& material) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* rec = db_->find(asset);
    if (!rec || rec->kind != assets::AssetKind::Material)
        return make_error(ErrorCode::NotFound, "material asset " + asset.to_string() + " not found");
    const fs::Path path = db_->source_path(*rec);
    VICTUS_TRY_STATUS(assets::save_material_file(path, material));
    if (auto text = fs::read_text(path); text) written_materials_[rec->path] = fnv1a64(*text);
    if (cache_) cache_->evict_materials();
    return {};
}

Result<Uuid> Editor::create_surface_shader(std::string_view relative_path) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const std::string ext(assets::kShaderExtension);
    std::string rel(relative_path);
    if (rel.empty()) {
        rel = "shaders/Surface" + ext;
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i) rel = std::format("shaders/Surface_{}{}", i, ext);
    } else if (!rel.ends_with(ext)) {
        rel += ext;
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    if (sfs::exists(db_->assets_dir() / *clean)) return make_error(ErrorCode::AlreadyExists, *clean + " already exists");
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    const std::string_view text = runtime::surface_shader_template();
    VICTUS_TRY_STATUS(fs::write_atomic(db_->assets_dir() / *clean,
                                       std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "shader was written but not registered: " + *clean);
    notify("Created surface shader " + *clean + " (edit it in any text editor; changes reload live)");
    return rec->uuid;
}

bool Editor::set_editing_material_shader(const Uuid& shader) {
    if (!db_ || material_editing_.is_nil()) return false;
    if (!shader.is_nil()) {
        const assets::AssetRecord* rec = db_->find(shader);
        if (!rec || rec->kind != assets::AssetKind::Shader) return false;
    }
    material_edit_.shader = shader;
    if (auto st = save_material(material_editing_, material_edit_); !st) {
        notify("Material save failed: " + st.error().message, true);
        return false;
    }
    return true;
}

bool Editor::assign_material(Entity e, const Uuid& material) {
    Scene& s = active_scene();
    auto* mr = s.world().alive(e) ? s.world().get<MeshRendererComponent>(e) : nullptr;
    if (!mr) return false;
    if (!material.is_nil()) {
        const assets::AssetRecord* rec = db_ ? db_->find(material) : nullptr;
        if (!rec || rec->kind != assets::AssetKind::Material) return false;
    }
    if (mr->material_asset == material) return true;
    auto before = capture(s, {s.uuid(e)});
    s.world().get<MeshRendererComponent>(e)->material_asset = material;
    record_edit(material.is_nil() ? "Clear material" : "Assign material", std::move(before));
    return true;
}

void Editor::open_material_editor(const Uuid& asset) {
    if (!db_) return;
    auto m = db_->load_material(asset);
    if (!m) {
        notify(m.error().message, true);
        return;
    }
    material_editing_ = asset;
    material_edit_ = *m;
    material_dirty_ = false;
    show_material_editor_ = true;
}

void Editor::draw_material_editor() {
    if (!show_material_editor_) return;
    ImGui::SetNextWindowSize(ImVec2(390, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material Editor", &show_material_editor_)) {
        ImGui::End();
        return;
    }
    const assets::AssetRecord* rec = db_ && !material_editing_.is_nil() ? db_->find(material_editing_) : nullptr;
    if (!rec || rec->kind != assets::AssetKind::Material) {
        ImGui::TextDisabled("Double-click a material in Assets, or use Assets > New Material.");
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(rec->path.c_str());
    ImGui::Separator();
    assets::MaterialAsset& m = material_edit_;
    bool changed = false;

    auto color = [&](const char* label, math::Color& c, bool alpha) {
        float v[4] = {c.r, c.g, c.b, c.a};
        const bool ch = alpha ? ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_Float)
                              : ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float);
        if (ch) c = {v[0], v[1], v[2], alpha ? v[3] : 1.0f};
        return ch;
    };
    auto texture_slot = [&](int slot) {
        Uuid& ref = *m.textures()[static_cast<std::size_t>(slot)];
        ImGui::PushID(slot);
        const assets::AssetRecord* t = ref.is_nil() ? nullptr : db_->find(ref);
        const std::string label = ref.is_nil() ? "(none)" : t ? t->path : "(missing) " + ref.to_string();
        ImGui::Text("%s:", assets::kMaterialTextureSlotNames[slot]);
        ImGui::SameLine();
        ImGui::Button(label.c_str(), ImVec2(-60, 0));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a texture from Assets here");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* r = db_->find(id); r && r->kind == assets::AssetKind::Texture) {
                    ref = id;
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear") && !ref.is_nil()) {
            ref = {};
            changed = true;
        }
        // Data textures must be imported as linear; sRGB decoding would skew normals and roughness.
        if (t && slot != 0 && slot != 3 && t->settings["srgb"].as_bool(true))
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "  imported as sRGB; set srgb = false in its .meta");
        ImGui::PopID();
    };

    if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= color("Base Color", m.base_color, true);
        texture_slot(0);
        changed |= ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f);
        texture_slot(1);
        texture_slot(2);
        changed |= ImGui::DragFloat("Normal Scale", &m.normal_scale, 0.01f, 0.0f, 4.0f);
    }
    if (ImGui::CollapsingHeader("Emission")) {
        changed |= color("Emissive", m.emissive, false);
        changed |= ImGui::DragFloat("Strength", &m.emissive_strength, 0.05f, 0.0f, 100.0f);
        texture_slot(3);
    }
    if (ImGui::CollapsingHeader("Texture Coordinates", ImGuiTreeNodeFlags_DefaultOpen)) {
        float sc[2] = {m.uv_scale.x, m.uv_scale.y}, of[2] = {m.uv_offset.x, m.uv_offset.y};
        if (ImGui::DragFloat2("Tiling", sc, 0.05f, -1000.0f, 1000.0f)) {
            m.uv_scale = {sc[0], sc[1]};
            changed = true;
        }
        if (ImGui::DragFloat2("Offset", of, 0.01f)) {
            m.uv_offset = {of[0], of[1]};
            changed = true;
        }
        int filter = m.filter == assets::TextureFilter::Nearest ? 1 : 0;
        if (ImGui::Combo("Filter", &filter, "Linear (smooth)\0Nearest (pixel art)\0")) {
            m.filter = filter == 1 ? assets::TextureFilter::Nearest : assets::TextureFilter::Linear;
            changed = true;
        }
    }
    if (ImGui::CollapsingHeader("Shader", ImGuiTreeNodeFlags_DefaultOpen)) {
        const assets::AssetRecord* sh = m.shader.is_nil() ? nullptr : db_->find(m.shader);
        const std::string label = m.shader.is_nil() ? "(built-in PBR)" : sh ? sh->path : "(missing) " + m.shader.to_string();
        ImGui::TextUnformatted("Surface:");
        ImGui::SameLine();
        ImGui::Button(label.c_str(), ImVec2(-100, 0));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a .vshader surface shader from Assets here");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* r = db_->find(id); r && r->kind == assets::AssetKind::Shader) {
                    m.shader = id;
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("New")) {
            if (auto id = create_surface_shader(); id) {
                m.shader = *id;
                if (m.shader_params[0] == math::Vec4{}) m.shader_params[0] = {1.0f, 0.5f, 0.1f, 2.0f}; // template: colour + speed
                changed = true;
            } else {
                notify("cannot create shader: " + id.error().message, true);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##shader") && !m.shader.is_nil()) {
            m.shader = {};
            changed = true;
        }
        if (sh && cache_) {
            // Compile status as the renderer sees it (the cache recompiles after hot reload).
            if (auto compiled = cache_->surface_shader(m.shader); !compiled)
                ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", compiled.error().message.c_str());
            else
                ImGui::TextDisabled("compiled; saved edits reload live");
        }
        for (int i = 0; i < 4; ++i) {
            math::Vec4& p = m.shader_params[static_cast<std::size_t>(i)];
            float v[4] = {p.x, p.y, p.z, p.w};
            if (ImGui::DragFloat4(std::format("params[{}]", i).c_str(), v, 0.01f)) {
                p = {v[0], v[1], v[2], v[3]};
                changed = true;
            }
        }
    }
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("Alpha Cutoff", &m.alpha_cutoff, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("Unlit", &m.unlit);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Double Sided", &m.double_sided);
    }
    ImGui::Separator();
    const Entity sel = selected();
    const auto* mr = sel.is_null() ? nullptr : active_scene().world().get<MeshRendererComponent>(sel);
    ImGui::BeginDisabled(!mr || mr->material_asset == material_editing_);
    if (ImGui::Button("Assign to Selected")) (void)assign_material(sel, material_editing_);
    ImGui::EndDisabled();

    material_dirty_ |= changed;
    // Save once the user lets go of a slider/colour (not on every drag step); the viewport updates
    // from the saved file, so what you see is what ships.
    if (material_dirty_ && !ImGui::IsAnyItemActive()) {
        if (auto st = save_material(material_editing_, m); !st) notify("Material save failed: " + st.error().message, true);
        material_dirty_ = false;
    }
    ImGui::End();
}

void Editor::new_material_for_selection() {
    const Entity e = selected();
    std::string base = "Material";
    if (!e.is_null())
        if (const auto* n = active_scene().world().get<Name>(e)) base = clean_base(n->value, "Material");
    std::string rel = "materials/" + base + std::string(assets::kMaterialExtension);
    for (int i = 2; db_ && sfs::exists(db_->assets_dir() / rel); ++i)
        rel = std::format("materials/{}_{}{}", base, i, assets::kMaterialExtension);
    assets::MaterialAsset m;
    if (const auto* mr = e.is_null() ? nullptr : active_scene().world().get<MeshRendererComponent>(e)) {
        // Start from what the entity looks like now, so assigning the new material changes nothing.
        m.base_color = mr->base_color;
        m.metallic = mr->metallic;
        m.roughness = mr->roughness;
    }
    auto id = create_material(rel, m);
    if (!id) {
        notify("cannot create material: " + id.error().message, true);
        return;
    }
    if (!e.is_null() && active_scene().world().has<MeshRendererComponent>(e)) {
        // Keep the look: the material now carries the colour, the entity tint goes back to white.
        auto before = capture(active_scene(), {active_scene().uuid(e)});
        auto* mr = active_scene().world().get<MeshRendererComponent>(e);
        mr->material_asset = *id;
        mr->base_color = {1, 1, 1, 1};
        record_edit("Assign material", std::move(before));
    }
    open_material_editor(*id);
}

} // namespace victus::editor
