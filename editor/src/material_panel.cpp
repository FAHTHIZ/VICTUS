// SPDX-License-Identifier: Apache-2.0
// Material assets in the editor: creating .vmat files, assigning them to mesh renderers (undoable)
// and the Material Editor window, which edits one material file with live viewport feedback.
// Material files are project assets, not scene data: their edits are saved to disk directly and are
// not part of scene undo (like prefab files).
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "editor_client.hpp"

#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/graph/material_graph.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
    if (thumbnails_) {
        // Material instances inherit from the saved one, and prefabs may wear it.
        thumbnails_->invalidate_kind(assets::AssetKind::Material);
        thumbnails_->invalidate_kind(assets::AssetKind::Prefab);
    }
    return {};
}

Result<Uuid> Editor::create_material_instance(const Uuid& parent, std::string_view relative_path) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* p = db_->find(parent);
    if (!p || p->kind != assets::AssetKind::Material)
        return make_error(ErrorCode::NotFound, "material asset " + parent.to_string() + " not found");
    std::string rel(relative_path);
    if (rel.empty()) {
        const fs::Path src(p->path);
        const std::string dir = src.parent_path().empty() ? std::string() : src.parent_path().generic_string() + "/";
        const std::string stem = src.stem().string();
        rel = dir + stem + "_instance" + std::string(assets::kMaterialExtension);
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i)
            rel = std::format("{}{}_instance_{}{}", dir, stem, i, assets::kMaterialExtension);
    }
    assets::MaterialAsset m;
    m.parent = parent;
    m.overrides = 0; // inherits everything
    return create_material(rel, m);
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
    // Instances are edited with their inherited values shown; a broken parent chain still opens
    // the file itself so it can be repaired.
    auto m = db_->resolve_material(asset);
    if (!m) {
        notify(m.error().message, true);
        m = db_->load_material(asset);
        if (!m) return;
    }
    material_editing_ = asset;
    material_edit_ = *m;
    material_dirty_ = false;
    show_material_editor_ = true;
}

void Editor::draw_material_editor() {
    if (!show_material_editor_) return;
    ImGui::SetNextWindowSize(ImVec2(430, 620), ImGuiCond_FirstUseEver);
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
    assets::MaterialAsset& m = material_edit_;
    bool changed = false;
    using F = assets::MaterialField;
    // Instances: editing a field overrides it; the "x" next to an overridden field goes back to
    // the parent's value. Standalone materials own every field.
    const bool instance = m.is_instance();
    auto mark = [&](bool ch, F f) {
        if (!ch) return false;
        m.overrides |= assets::field_bit(f);
        changed = true;
        return true;
    };
    auto inherit_button = [&](F f) {
        if (!instance) return;
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(f) + 1000);
        if (m.overrides_field(f)) {
            if (ImGui::SmallButton("x")) {
                if (auto parent = db_->resolve_material(m.parent); parent) m.copy_field(*parent, f);
                m.overrides &= ~assets::field_bit(f);
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overridden here; click to use the parent's value");
        } else {
            ImGui::TextDisabled("inherited");
        }
        ImGui::PopID();
    };
    if (instance) {
        const assets::AssetRecord* p = db_->find(m.parent);
        ImGui::TextDisabled("Instance of %s", p ? p->path.c_str() : ("(missing) " + m.parent.to_string()).c_str());
        if (p) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit Parent")) {
                const Uuid parent_id = m.parent;
                open_material_editor(parent_id);
                ImGui::End();
                return;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Detach")) {
            m.parent = {}; // keeps the current (resolved) values as its own
            m.overrides = assets::kAllMaterialFields;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Make this a standalone material with the values shown");
    }
    ImGui::Separator();
    // Instances need room for the inherited/override marker after each field.
    ImGui::PushItemWidth(instance ? ImGui::GetContentRegionAvail().x * 0.48f : ImGui::GetContentRegionAvail().x * 0.62f);

    auto color = [&](const char* label, math::Color& c, bool alpha) {
        float v[4] = {c.r, c.g, c.b, c.a};
        const bool ch = alpha ? ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_Float)
                              : ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float);
        if (ch) c = {v[0], v[1], v[2], alpha ? v[3] : 1.0f};
        return ch;
    };
    constexpr F kSlotFields[4] = {F::BaseColorTexture, F::MetallicRoughnessTexture, F::NormalTexture, F::EmissiveTexture};
    auto texture_slot = [&](int slot) {
        const F field = kSlotFields[slot];
        Uuid& ref = *m.textures()[static_cast<std::size_t>(slot)];
        ImGui::PushID(slot);
        const assets::AssetRecord* t = ref.is_nil() ? nullptr : db_->find(ref);
        const std::string label = ref.is_nil() ? "(none)" : t ? t->path : "(missing) " + ref.to_string();
        ImGui::Text("%s:", assets::kMaterialTextureSlotNames[slot]);
        ImGui::SameLine();
        ImGui::Button(label.c_str(), ImVec2(instance ? -150.0f : -60.0f, 0.0f));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a texture from Assets here");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* r = db_->find(id); r && r->kind == assets::AssetKind::Texture) {
                    ref = id;
                    mark(true, field);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear") && !ref.is_nil()) {
            ref = {};
            mark(true, field);
        }
        inherit_button(field);
        // Data textures must be imported as linear; sRGB decoding would skew normals and roughness.
        if (t && slot != 0 && slot != 3 && t->settings["srgb"].as_bool(true))
            ImGui::TextColored(ui::tokens::kWarning, "  imported as sRGB; set srgb = false in its .meta");
        ImGui::PopID();
    };

    if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
        mark(color("Base Color", m.base_color, true), F::BaseColor);
        inherit_button(F::BaseColor);
        texture_slot(0);
        mark(ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f), F::Metallic);
        inherit_button(F::Metallic);
        mark(ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f), F::Roughness);
        inherit_button(F::Roughness);
        mark(ImGui::SliderFloat("Occlusion", &m.occlusion_strength, 0.0f, 1.0f), F::OcclusionStrength);
        ImGui::SetItemTooltip("Ambient occlusion from the red channel of the metallic-roughness (ORM) texture");
        inherit_button(F::OcclusionStrength);
        texture_slot(1);
        texture_slot(2);
        mark(ImGui::DragFloat("Normal Scale", &m.normal_scale, 0.01f, 0.0f, 4.0f), F::NormalScale);
        inherit_button(F::NormalScale);
        mark(ImGui::SliderFloat("Clear Coat", &m.clearcoat, 0.0f, 1.0f), F::Clearcoat);
        ImGui::SetItemTooltip("A glossy transparent layer over the surface: lacquer, car paint, glazed ceramic, varnish");
        inherit_button(F::Clearcoat);
        if (m.clearcoat > 0.0f) {
            mark(ImGui::SliderFloat("Coat Roughness", &m.clearcoat_roughness, 0.0f, 1.0f), F::ClearcoatRoughness);
            ImGui::SetItemTooltip("How blurry the coat's reflection is, independent of the surface underneath");
            inherit_button(F::ClearcoatRoughness);
        }
        mark(ImGui::ColorEdit3("Sheen", &m.sheen_color.x, ImGuiColorEditFlags_Float), F::SheenColor);
        ImGui::SetItemTooltip("Soft glow at grazing angles from fibres and fuzz: velvet, cotton, felt, peach skin. Black = off");
        inherit_button(F::SheenColor);
        if (m.sheen_color != math::Vec3{}) {
            mark(ImGui::SliderFloat("Sheen Roughness", &m.sheen_roughness, 0.0f, 1.0f), F::SheenRoughness);
            ImGui::SetItemTooltip("Low: a thin bright rim (satin, silk). High: a wide soft haze (velvet, wool)");
            inherit_button(F::SheenRoughness);
        }
        mark(ImGui::SliderFloat("Anisotropy", &m.anisotropy, 0.0f, 1.0f), F::Anisotropy);
        ImGui::SetItemTooltip("Stretches highlights along one direction: brushed metal, hair, satin, machined parts");
        inherit_button(F::Anisotropy);
        if (m.anisotropy > 0.0f) {
            mark(ImGui::SliderAngle("Anisotropy Rotation", &m.anisotropy_rotation, -180.0f, 180.0f), F::AnisotropyRotation);
            ImGui::SetItemTooltip("Turns the stretch direction away from the mesh tangent");
            inherit_button(F::AnisotropyRotation);
        }
        mark(ImGui::ColorEdit3("Subsurface", &m.subsurface_color.x, ImGuiColorEditFlags_Float), F::SubsurfaceColor);
        ImGui::SetItemTooltip("Light scattering inside the material: skin, wax, marble, jade, leaves. Black = off");
        inherit_button(F::SubsurfaceColor);
        if (m.subsurface_color != math::Vec3{}) {
            mark(ImGui::SliderFloat("Thickness", &m.subsurface_thickness, 0.0f, 1.0f), F::SubsurfaceThickness);
            ImGui::SetItemTooltip("1: a solid body (softer shadow edge only). 0: thin, light shines through (ears, leaves)");
            inherit_button(F::SubsurfaceThickness);
        }
    }
    if (ImGui::CollapsingHeader("Emission")) {
        mark(color("Emissive", m.emissive, false), F::Emissive);
        inherit_button(F::Emissive);
        mark(ImGui::DragFloat("Strength", &m.emissive_strength, 0.05f, 0.0f, 100.0f), F::EmissiveStrength);
        inherit_button(F::EmissiveStrength);
        texture_slot(3);
    }
    if (ImGui::CollapsingHeader("Texture Coordinates", ImGuiTreeNodeFlags_DefaultOpen)) {
        float sc[2] = {m.uv_scale.x, m.uv_scale.y}, of[2] = {m.uv_offset.x, m.uv_offset.y};
        if (mark(ImGui::DragFloat2("Tiling", sc, 0.05f, -1000.0f, 1000.0f), F::UvScale)) m.uv_scale = {sc[0], sc[1]};
        inherit_button(F::UvScale);
        if (mark(ImGui::DragFloat2("Offset", of, 0.01f), F::UvOffset)) m.uv_offset = {of[0], of[1]};
        inherit_button(F::UvOffset);
        mark(ImGui::Checkbox("Triplanar (world space)", &m.triplanar), F::Triplanar);
        ImGui::SetItemTooltip("Project the textures along the world axes instead of using the mesh's UVs, so scaled "
                              "blocks and rocks tile evenly. Tiling then counts repeats per metre.");
        inherit_button(F::Triplanar);
        if (m.triplanar) {
            mark(ImGui::SliderFloat("Blend Sharpness", &m.triplanar_sharpness, 1.0f, 64.0f, "%.1f",
                                    ImGuiSliderFlags_Logarithmic),
                 F::TriplanarSharpness);
            ImGui::SetItemTooltip("How quickly one projection gives way to the next on curved surfaces");
            inherit_button(F::TriplanarSharpness);
        }
        mark(ImGui::SliderFloat("Anti-Tiling", &m.anti_tiling, 0.0f, 1.0f), F::AntiTiling);
        ImGui::SetItemTooltip("Breaks up visible texture repeats: large-scale brightness variation, and above 0.5 a "
                              "larger copy of the texture blended in with distance (for grass, rock, dirt)");
        inherit_button(F::AntiTiling);
        int filter = m.filter == assets::TextureFilter::Nearest ? 1 : 0;
        if (mark(ImGui::Combo("Filter", &filter, "Linear (smooth)\0Nearest (pixel art)\0"), F::Filter))
            m.filter = filter == 1 ? assets::TextureFilter::Nearest : assets::TextureFilter::Linear;
        inherit_button(F::Filter);
    }
    if (ImGui::CollapsingHeader("Shader", ImGuiTreeNodeFlags_DefaultOpen)) {
        const assets::AssetRecord* sh = m.shader.is_nil() ? nullptr : db_->find(m.shader);
        const std::string label = m.shader.is_nil() ? "(built-in PBR)" : sh ? sh->path : "(missing) " + m.shader.to_string();
        ImGui::TextUnformatted("Surface:");
        ImGui::SameLine();
        ImGui::Button(label.c_str(), ImVec2(-100, 0));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a .vshader surface shader or .vmatgraph material graph from Assets here");
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* r = db_->find(id); r && r->kind == assets::AssetKind::Shader) {
                    m.shader = id;
                    mark(true, F::Shader);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("New")) {
            if (auto id = create_surface_shader(); id) {
                m.shader = *id;
                mark(true, F::Shader); // the template's parameters take their declared defaults
            } else {
                notify("cannot create shader: " + id.error().message, true);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Graph")) {
            if (auto id = create_material_graph(); id) {
                m.shader = *id;
                mark(true, F::Shader);
                if (auto st = open_graph_editor(*id); !st.ok()) notify(st.error().message, true);
            } else {
                notify("cannot create material graph: " + id.error().message, true);
            }
        }
        ImGui::SetItemTooltip("New node-based material graph as this material's shader");
        if (sh && sh->path.ends_with(graph::kMaterialGraphExtension)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit Graph"))
                if (auto st = open_graph_editor(m.shader); !st.ok()) notify(st.error().message, true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##shader") && !m.shader.is_nil()) {
            m.shader = {};
            mark(true, F::Shader);
        }
        inherit_button(F::Shader);
        if (sh && cache_) {
            // Compile status as the renderer sees it (the cache recompiles after hot reload).
            if (auto compiled = cache_->surface_shader(m.shader); !compiled)
                ImGui::TextColored(ui::tokens::kDanger, "%s", compiled.error().message.c_str());
            else
                ImGui::TextDisabled("compiled; saved edits reload live");
        }
        // Named parameters and textures declared by the shader (ADR-0171); raw slots otherwise.
        const render::SurfaceDeclarations* decl = sh && cache_ ? cache_->surface_declarations(m.shader) : nullptr;
        const bool named = decl && (!decl->params.empty() || !decl->textures.empty());
        // Marks slot `slot` as set; earlier unset slots get the shader's defaults so they keep their look.
        auto set_slot = [&](std::size_t slot) {
            if (decl)
                for (const render::SurfaceParamDecl& d : decl->params)
                    if (d.slot < slot && !m.shader_param_set(d.slot)) m.shader_params[d.slot] = d.value;
            m.shader_params_set.count = static_cast<std::uint8_t>(std::max<std::size_t>(m.shader_params_set.count, slot + 1));
        };
        if (named) {
            for (const render::SurfaceParamDecl& d : decl->params) {
                const math::Vec4 cur = m.shader_param_set(d.slot) ? m.shader_params[d.slot] : d.value;
                float v[4] = {cur.x, cur.y, cur.z, cur.w};
                const int n = d.components;
                bool ch = false;
                if (d.color) ch = n == 4 ? ImGui::ColorEdit4(d.name.c_str(), v, ImGuiColorEditFlags_Float)
                                         : ImGui::ColorEdit3(d.name.c_str(), v, ImGuiColorEditFlags_Float);
                else if (d.has_range) ch = ImGui::SliderScalarN(d.name.c_str(), ImGuiDataType_Float, v, n, &d.min, &d.max);
                else ch = ImGui::DragScalarN(d.name.c_str(), ImGuiDataType_Float, v, n, 0.01f);
                if (d.has_range && d.step > 0.0f)
                    for (float& f : v) f = d.min + std::round((f - d.min) / d.step) * d.step;
                if (mark(ch, F::ShaderParams)) {
                    set_slot(d.slot);
                    m.shader_params[d.slot] = {v[0], v[1], v[2], v[3]};
                }
            }
            if (!decl->params.empty()) inherit_button(F::ShaderParams);
            for (const render::SurfaceTextureDecl& d : decl->textures) {
                Uuid& ref = m.shader_textures[d.slot];
                ImGui::PushID(100 + d.slot);
                const assets::AssetRecord* t = ref.is_nil() ? nullptr : db_->find(ref);
                static constexpr const char* kDefaults[] = {"(white)", "(black)", "(flat normal)"};
                const std::string slot_label = ref.is_nil() ? kDefaults[static_cast<int>(d.fallback)] : t ? t->path : "(missing) " + ref.to_string();
                ImGui::Text("%s:", d.name.c_str());
                ImGui::SameLine();
                ImGui::Button(slot_label.c_str(), ImVec2(instance ? -150.0f : -60.0f, 0.0f));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a texture from Assets here");
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                        Uuid id;
                        std::memcpy(&id, pl->Data, sizeof(Uuid));
                        if (const auto* r = db_->find(id); r && r->kind == assets::AssetKind::Texture) {
                            ref = id;
                            mark(true, F::ShaderTextures);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear") && !ref.is_nil()) {
                    ref = {};
                    mark(true, F::ShaderTextures);
                }
                ImGui::PopID();
            }
            if (!decl->textures.empty()) inherit_button(F::ShaderTextures);
        }
        if (!named || ImGui::TreeNode("Raw parameter slots")) {
            for (int i = 0; i < 4; ++i) {
                math::Vec4& p = m.shader_params[static_cast<std::size_t>(i)];
                float v[4] = {p.x, p.y, p.z, p.w};
                if (mark(ImGui::DragFloat4(std::format("params[{}]", i).c_str(), v, 0.01f), F::ShaderParams)) {
                    set_slot(static_cast<std::size_t>(i));
                    p = {v[0], v[1], v[2], v[3]};
                }
            }
            if (!named) inherit_button(F::ShaderParams); // the four rows are one field
            else ImGui::TreePop();
        }
    }
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        mark(ImGui::SliderFloat("Alpha Cutoff", &m.alpha_cutoff, 0.0f, 1.0f), F::AlphaCutoff);
        inherit_button(F::AlphaCutoff);
        mark(ImGui::Checkbox("Unlit", &m.unlit), F::Unlit);
        inherit_button(F::Unlit);
        mark(ImGui::Checkbox("Double Sided", &m.double_sided), F::DoubleSided);
        inherit_button(F::DoubleSided);
        int blend = static_cast<int>(m.blend);
        if (mark(ImGui::Combo("Blend", &blend, "Opaque\0Alpha (glass, smoke)\0Additive (glow)\0"), F::Blend))
            m.blend = static_cast<assets::MaterialBlendMode>(std::clamp(blend, 0, 2));
        ImGui::SetItemTooltip("Blended materials draw after everything opaque, far to near, and do not cast shadows");
        inherit_button(F::Blend);
        if (m.blend != assets::MaterialBlendMode::Opaque) {
            mark(ImGui::SliderFloat("Responsive", &m.responsive, 0.0f, 1.0f), F::Responsive);
            ImGui::SetItemTooltip("Raise for moving glass, holograms or screens that leave trails with TAA: anti-aliasing trusts the\n"
                                  "current frame where this surface covers the screen (0 = smoothest, 1 = no trails)");
            inherit_button(F::Responsive);
        }
        mark(ImGui::SliderFloat("Coverage Response", &m.coverage_response, 0.0f, 1.0f), F::CoverageResponse);
        ImGui::SetItemTooltip("How much the scene's snow or dust layer settles on this material (0 = never)");
        inherit_button(F::CoverageResponse);
    }
    ImGui::PopItemWidth();
    ImGui::Separator();
    const Entity sel = selected();
    const auto* mr = sel.is_null() ? nullptr : active_scene().world().get<MeshRendererComponent>(sel);
    ImGui::BeginDisabled(!mr || mr->material_asset == material_editing_);
    if (ImGui::Button("Assign to Selected")) (void)assign_material(sel, material_editing_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Create Instance")) {
        if (auto id = create_material_instance(material_editing_); id) {
            open_material_editor(*id);
        } else {
            notify("cannot create instance: " + id.error().message, true);
        }
        ImGui::End();
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New material that inherits everything from this one; override what differs");

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
