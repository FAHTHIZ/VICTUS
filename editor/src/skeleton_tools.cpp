// SPDX-License-Identifier: Apache-2.0
// The Inspector's "Skeleton and skinning" card (ADR-0206): the selected character's skeleton
// (bones, inferred roles, skin joints, weighted vertices, sockets), its skinning method (linear
// or dual quaternion, one undo step), a deformation check that measures both methods under a
// twisted, bent limb, and bone creation written as a versioned skeleton migration next to the
// model, so the source file and every existing joint, socket and clip stay untouched.
#include "victus/editor/editor.hpp"
#include "victus/render/skinning.hpp"
#include "widgets.hpp"

#include <imgui.h>

#include <format>

namespace victus::editor {

using namespace scene;
namespace tk = ui::tokens;

namespace {
void subtree(const Scene& s, Entity e, std::vector<Entity>& out, int depth) {
    if (depth > 64) return;
    out.push_back(e);
    for (const Entity c : s.children(e)) subtree(s, c, out, depth + 1);
}
std::vector<Entity> subtree(const Scene& s, Entity root) {
    std::vector<Entity> out;
    if (s.world().alive(root)) subtree(s, root, out, 0);
    return out;
}
} // namespace

Uuid Editor::skinned_model_of(Entity root) {
    Scene& s = active_scene();
    for (const Entity e : subtree(s, root))
        if (s.world().get<animation::SkinnedMeshComponent>(e))
            if (const auto* mr = s.world().get<MeshRendererComponent>(e); mr && !mr->mesh_asset.is_nil()) return mr->mesh_asset;
    return {};
}

std::size_t Editor::set_skinning_method(Entity root, animation::SkinningMode mode) {
    Scene& s = active_scene();
    std::vector<Uuid> ids;
    for (const Entity e : subtree(s, root))
        if (const auto* sm = s.world().get<animation::SkinnedMeshComponent>(e); sm && sm->method != mode) ids.push_back(s.uuid(e));
    if (ids.empty()) return 0;
    auto before = capture(s, ids);
    for (const Uuid& u : ids) s.world().get<animation::SkinnedMeshComponent>(s.find(u))->method = mode;
    record_edit(mode == animation::SkinningMode::DualQuaternion ? "Dual Quaternion Skinning" : "Linear Skinning", std::move(before));
    return ids.size();
}

Result<std::string> Editor::add_skeleton_migration(const Uuid& model, std::span<const assets::BoneEdit> edits, std::string description) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    const assets::AssetRecord* rec = db_->find(model);
    if (!rec || rec->kind != assets::AssetKind::Model) return make_error(ErrorCode::NotFound, "not a model asset");
    if (edits.empty()) return make_error(ErrorCode::InvalidArgument, "no skeleton edits");
    auto current = db_->load_model_geometry(model);
    if (!current) return std::move(current).error();
    assets::ModelImportSettings settings = assets::ModelImportSettings::from_json(rec->settings);
    assets::SkeletonMigration mig;
    mig.profile = fs::Path(rec->path).stem().string();
    mig.from_version = static_cast<std::uint32_t>(settings.skeleton_migrations.size() + 1);
    mig.to_version = mig.from_version + 1;
    mig.requires_layout = assets::skeleton_layout_hash(*current);
    mig.description = std::move(description);
    mig.edits.assign(edits.begin(), edits.end());
    // Dry run on a copy: an unsafe edit is reported and nothing is written.
    assets::CookedModel trial = *current;
    if (auto r = assets::apply_skeleton_migration(trial, mig); !r) return std::move(r).error();
    const std::string file = std::format("{}.rig{}.vrig", fs::Path(rec->path).filename().string(), mig.to_version);
    const fs::Path dir = fs::Path(rec->path).parent_path();
    const std::string rel = (dir / file).generic_string();
    if (auto st = fs::write_text_atomic(db_->assets_dir() / dir / file, assets::write_skeleton_migration(mig)); !st)
        return std::move(st).error();
    settings.skeleton_migrations.push_back(file); // relative to the model's directory
    const Uuid uuid = model;
    if (auto st = db_->set_settings(uuid, settings.to_json()); !st) return std::move(st).error();
    if (auto imported = db_->import(uuid); !imported) return std::move(imported).error();
    skeleton_card_.stamp = 0; // re-read
    notify(std::format("Skeleton migration {} added ({} edits). The source model is unchanged.", rel, edits.size()));
    return rel;
}

Result<Editor::DeformationCheck> Editor::check_deformation(const Uuid& model, std::string_view bone, float roll_degrees,
                                                           float bend_degrees) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    auto m = db_->load_model_geometry(model);
    if (!m) return std::move(m).error();
    const assets::SkeletonInfo info = assets::describe_skeleton(*m);
    const assets::SkeletonBone* b = info.find(bone);
    if (!b) return make_error(ErrorCode::NotFound, std::format("no bone '{}'", bone));
    constexpr float kDeg = 3.14159265f / 180.0f;
    const std::pair<std::uint32_t, math::Quat> pose[1] = {
        {static_cast<std::uint32_t>(b->node),
         m->nodes[static_cast<std::size_t>(b->node)].rotation * math::Quat::axis_angle({0, 1, 0}, roll_degrees * kDeg) *
             math::Quat::axis_angle({1, 0, 0}, bend_degrees * kDeg)}};
    const auto rest = assets::pose_palette(*m, assets::pose_world(*m, nullptr, 0));
    const auto posed = assets::pose_palette(*m, assets::pose_world(*m, nullptr, 0, pose));
    DeformationCheck out;
    out.bone = std::string(bone);
    std::vector<render::Vertex3D> v0, v1, v2;
    for (const auto& mesh : m->meshes) {
        if (!mesh.data.skinned()) continue;
        render::skin_vertices(mesh.data, rest, render::SkinningMethod::Linear, v0);
        render::skin_vertices(mesh.data, posed, render::SkinningMethod::Linear, v1);
        render::skin_vertices(mesh.data, posed, render::SkinningMethod::DualQuaternion, v2);
        out.collapsed_linear += render::measure_deformation(v0, v1, mesh.data.indices).collapsed_triangles;
        out.collapsed_dual_quaternion += render::measure_deformation(v0, v2, mesh.data.indices).collapsed_triangles;
        out.triangles += mesh.data.indices.size() / 3;
    }
    return out;
}

void Editor::draw_skeleton_section(Entity e) {
    Scene& s = active_scene();
    const Uuid model = db_ ? skinned_model_of(e) : Uuid{};
    if (model.is_nil()) return;
    SkeletonCardState& st = skeleton_card_;
    if (st.model != model || st.stamp == 0) {
        st = SkeletonCardState{};
        st.model = model;
        st.stamp = 1;
        // Weighted vertex counts need the geometry; read once per model (and after a migration).
        if (auto g = db_->load_model_geometry(model)) st.info = assets::describe_skeleton(*g);
        else st.error = g.error().message;
    }
    ImGui::PushID("##skeleton_card");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tk::kSurfaceRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tk::kRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace4, tk::kSpace3));
    ImGui::BeginChild("##skeleton", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    kit::icon(kit::Icon::Animation, tk::kAccent);
    ImGui::SameLine();
    ImGui::PushFont(kit::bold_font(), 0.0f);
    ImGui::TextUnformatted("Skeleton and skinning");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    if (!st.error.empty()) ImGui::TextColored(tk::kWarning, "%s", st.error.c_str());
    const assets::SkeletonInfo& info = st.info;
    ImGui::TextColored(tk::kTextMuted, "%u joints, %u sockets, %u helpers, depth %u", info.joints, info.sockets, info.helpers,
                       info.max_depth);
    if (info.unweighted_joints > 0)
        ImGui::TextColored(tk::kTextMuted, "%u joints carry no skin weights (fine for eyes, IK and roll helpers)", info.unweighted_joints);
    ImGui::PopTextWrapPos();

    // Skinning method of every skinned mesh under the selection.
    animation::SkinningMode mode = animation::SkinningMode::Linear;
    for (const Entity c : subtree(s, e))
        if (const auto* sm = s.world().get<animation::SkinnedMeshComponent>(c)) {
            mode = sm->method;
            break;
        }
    int m = mode == animation::SkinningMode::DualQuaternion ? 1 : 0;
    const char* modes[] = {"Linear (fastest)", "Dual quaternion (keeps volume)"};
    if (ImGui::Combo("Skinning", &m, modes, 2))
        (void)set_skinning_method(e, m == 1 ? animation::SkinningMode::DualQuaternion : animation::SkinningMode::Linear);
    kit::tooltip("Dual quaternion skinning keeps twisted forearms and bent elbows and knees from collapsing. "
                 "Same memory and vertex format; a little more GPU work per vertex.");

    if (ImGui::TreeNode("Bones")) {
        if (ImGui::BeginTable("##bones", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, 220))) {
            ImGui::TableSetupColumn("Bone");
            ImGui::TableSetupColumn("Roles");
            ImGui::TableSetupColumn("Joint");
            ImGui::TableSetupColumn("Verts");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < info.bones.size(); ++i) {
                const assets::SkeletonBone& b = info.bones[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Indent(static_cast<float>(b.depth) * 8.0f);
                if (ImGui::Selectable(b.name.c_str(), st.selected == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns))
                    st.selected = static_cast<int>(i);
                ImGui::Unindent(static_cast<float>(b.depth) * 8.0f);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", assets::bone_roles_string(b.roles).c_str());
                ImGui::TableNextColumn();
                if (b.joint >= 0) ImGui::Text("%d", b.joint);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (b.joint >= 0) ImGui::Text("%u", b.weighted_vertices);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    const assets::SkeletonBone* sel =
        st.selected >= 0 && static_cast<std::size_t>(st.selected) < info.bones.size() ? &info.bones[static_cast<std::size_t>(st.selected)] : nullptr;
    if (sel) {
        ImGui::Text("%s: %s, length %.3f m%s", sel->name.c_str(), sel->joint >= 0 ? "deforming joint" : "helper", sel->length,
                    sel->animated ? ", animated" : "");
        if (ImGui::TreeNode("Deformation check")) {
            ImGui::SliderFloat("Roll", &st.roll, 0.0f, 180.0f, "%.0f deg");
            ImGui::SliderFloat("Bend", &st.bend, 0.0f, 160.0f, "%.0f deg");
            if (kit::icon_text_button("Measure", kit::Icon::Play)) {
                auto r = check_deformation(model, sel->name, st.roll, st.bend);
                if (r) st.check = *r;
                else notify(r.error().message, true);
            }
            kit::tooltip("Poses the bone and counts triangles that shrink below 10% of their area with each skinning method.");
            if (st.check && st.check->bone == sel->name)
                ImGui::Text("Collapsed triangles: linear %zu, dual quaternion %zu (of %zu)", st.check->collapsed_linear,
                            st.check->collapsed_dual_quaternion, st.check->triangles);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Add child bone")) {
            ImGui::InputText("Name", st.new_bone, sizeof(st.new_bone));
            ImGui::DragFloat3("Offset", st.offset, 0.005f, -2.0f, 2.0f, "%.3f m");
            ImGui::Checkbox("Deforms the mesh", &st.deform);
            kit::tooltip("Off for IK targets, poles and equipment sockets.");
            if (st.deform && sel->joint >= 0) {
                ImGui::SliderFloat("Take weights", &st.weight_share, 0.0f, 1.0f, "%.2f");
                kit::tooltip("Share of the parent's weights the new bone takes, growing towards the end of the parent bone (roll and twist joints use 0.5).");
            }
            const bool ok = st.new_bone[0] != 0;
            if (kit::icon_text_button("Add as migration", kit::Icon::Plus, true, ok && play_state_ == PlayState::Editing)) {
                assets::BoneEdit be;
                be.op = assets::BoneEdit::Op::Add;
                be.bone = st.new_bone;
                be.parent = sel->name;
                be.translation = {st.offset[0], st.offset[1], st.offset[2]};
                be.deform = st.deform;
                if (st.deform && sel->joint >= 0 && st.weight_share > 0.0f) {
                    be.weights_from = sel->name;
                    be.weight_share = st.weight_share;
                }
                const assets::BoneEdit one[1] = {be};
                if (auto r = add_skeleton_migration(model, one, std::format("Add {} under {}", be.bone, be.parent)); !r)
                    notify(r.error().message, true);
            }
            kit::tooltip("Writes a skeleton migration next to the model and re-imports it. Existing joints, sockets and "
                         "clips keep working; delete the .vrig file (and its entry in the import settings) to undo.");
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Spacing();
}

} // namespace victus::editor
