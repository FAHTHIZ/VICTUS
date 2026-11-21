// SPDX-License-Identifier: Apache-2.0
// The Inspector's "Feet on the ground" card (ADR-0163): one undoable step adds foot placement
// and foot locking to a humanoid (animation::make_humanoid_foot_placement); a toggle for foot
// locking and removal. Shown under the ragdoll card for characters with a hips/pelvis bone.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include <imgui.h>

#include <format>

namespace victus::editor {

namespace tk = ui::tokens;
using namespace scene;

namespace {
void subtree_ids(const Scene& s, Entity e, std::vector<Uuid>& out, int depth) {
    if (depth > 128 || out.size() >= 4096) return;
    out.push_back(s.uuid(e));
    for (const Entity c : s.children(e)) subtree_ids(s, c, out, depth + 1);
}
std::vector<Uuid> subtree_ids(const Scene& s, Entity root) {
    std::vector<Uuid> out;
    if (s.world().alive(root)) subtree_ids(s, root, out, 0);
    return out;
}
} // namespace

std::size_t Editor::add_foot_placement(Entity root) {
    Scene& s = active_scene();
    if (!s.world().alive(root)) return 0;
    auto before = capture(s, subtree_ids(s, root));
    const std::size_t feet = animation::make_humanoid_foot_placement(s, root);
    if (feet == 0) {
        notify("No feet found: foot placement needs bones named like Foot.L / Foot.R (or ankle) with a leg above them.", true);
        return 0;
    }
    record_edit("Add Foot Placement", std::move(before));
    notify(std::format("Foot placement added to {} feet. Press Play: feet stand on slopes and steps and stop sliding.", feet));
    return feet;
}

std::size_t Editor::remove_foot_placement(Entity root) {
    Scene& s = active_scene();
    if (!s.world().alive(root) || !s.world().has<animation::FootPlacementComponent>(root)) return 0;
    auto before = capture(s, subtree_ids(s, root));
    const std::size_t removed = animation::remove_foot_placement(s, root);
    record_edit("Remove Foot Placement", std::move(before));
    return removed;
}

bool Editor::set_foot_locking(Entity root, bool on) {
    Scene& s = active_scene();
    auto* fp = s.world().alive(root) ? s.world().get<animation::FootPlacementComponent>(root) : nullptr;
    if (!fp) return false;
    if (fp->lock_feet == on) return true;
    auto before = capture(s, {s.uuid(root)});
    fp->lock_feet = on;
    record_edit(on ? "Foot Locking On" : "Foot Locking Off", std::move(before));
    return true;
}

std::size_t Editor::foot_contact_count(Entity root) const {
    const Scene& s = const_cast<Editor*>(this)->active_scene();
    std::vector<Uuid> ids = subtree_ids(s, root);
    std::size_t n = 0;
    for (const Uuid& u : ids)
        if (const Entity e = s.find(u); !e.is_null() && s.world().get<animation::FootContactComponent>(e)) ++n;
    return n;
}

void Editor::draw_foot_section(Entity e) {
    Scene& s = active_scene();
    auto* fp = s.world().get<animation::FootPlacementComponent>(e);
    if (!fp && !can_make_ragdoll(e)) return; // same "looks like a humanoid" test (a hips/pelvis bone)
    ImGui::PushID("##foot_card");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tk::kSurfaceRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tk::kRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace4, tk::kSpace3));
    ImGui::BeginChild("##feet", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    kit::icon(kit::Icon::Animation, tk::kAccent);
    ImGui::SameLine();
    ImGui::PushFont(kit::bold_font(), 0.0f);
    ImGui::TextUnformatted("Feet on the ground");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    if (!fp) {
        ImGui::TextColored(tk::kTextMuted,
                           "Plant the feet on slopes, steps and uneven ground, and keep planted feet from sliding.");
        ImGui::PopTextWrapPos();
        if (kit::icon_text_button("Add Foot Placement", kit::Icon::Plus, true, true, ImVec2(-1, 0))) (void)add_foot_placement(e);
        kit::tooltip("Adds leg IK to each foot and lowers the hips when one foot stands lower. Needs physics "
                     "colliders on the ground. Undo removes it again.");
    } else {
        const std::size_t feet = foot_contact_count(e);
        ImGui::SameLine();
        kit::pill(std::format("{} feet", feet), feet > 0 ? ui::Tone::Success : ui::Tone::Warning);
        if (feet == 0) ImGui::TextColored(tk::kWarning, "No feet yet: add FootContact and a two-bone IKConstraint to each foot bone.");
        if (play_state_ == PlayState::Editing) ImGui::TextColored(tk::kInfo, "Press Play to see the feet adapt to the ground.");
        ImGui::PopTextWrapPos();
        bool lock = fp->lock_feet;
        if (ImGui::Checkbox("Stop feet sliding", &lock)) (void)set_foot_locking(e, lock);
        kit::tooltip("A foot that stands still on the ground stays exactly where it planted until the animation lifts it.");
        ImGui::SameLine();
        if (kit::icon_text_button("Remove", kit::Icon::Close)) (void)remove_foot_placement(e);
        kit::tooltip("Removes foot placement and the leg IK it added.");
        ImGui::TextDisabled("Tune it in the FootPlacement component below; View > IK and Foot Placement shows the probes.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Spacing();
}

} // namespace victus::editor
