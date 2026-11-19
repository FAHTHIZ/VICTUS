// SPDX-License-Identifier: Apache-2.0
// Inspector > "Make it a...": one click turns the selected object into a player, enemy, coin,
// hazard, checkpoint or goal by adding the Behaviours that role needs (ADR-0120), plus the scene
// pieces it depends on (Game Rules, a camera that follows the player). One undo step.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/gameplay/gameplay.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>

#include <array>

namespace victus::editor {

namespace tk = ui::tokens;
using namespace scene;

namespace {

struct RoleInfo {
    Role role;
    const char* label;
    const char* name; // entity name given to objects that still have a default name
    const char* tooltip;
};

constexpr std::array<RoleInfo, 6> kRoles = {{
    {Role::Player, "Player", "Player", "Moves with the keyboard or a gamepad, has health and respawns. The camera follows it."},
    {Role::Enemy, "Enemy", "Enemy", "Walks back and forth and hurts the player; jump on it to defeat it."},
    {Role::Coin, "Coin", "Coin", "Adds 1 to the score when the player touches it."},
    {Role::Hazard, "Hazard", "Spikes", "Hurts the player on touch, like spikes or lava."},
    {Role::Checkpoint, "Checkpoint", "Checkpoint", "The player respawns here after reaching it."},
    {Role::Goal, "Goal", "Goal", "Reaching it wins the game."},
}};

bool default_name(std::string_view n) {
    for (const char* d : {"Entity", "Cube", "Sphere", "Plane", "Cylinder", "Sprite"})
        if (n == d || (n.starts_with(d) && n.size() > std::string_view(d).size() && n[std::string_view(d).size()] == ' '))
            return true;
    return false;
}

} // namespace

const char* role_label(Role r) noexcept {
    for (const auto& i : kRoles)
        if (i.role == r) return i.label;
    return "";
}

bool Editor::make_role(Entity e, Role role) {
    Scene& s = active_scene();
    if (e.is_null() || !s.world().alive(e)) return false;
    auto& w = s.world();
    const bool two_d = runtime::scene_view_mode(s) == runtime::ViewMode::TwoD;
    std::vector<Uuid> touched{s.uuid(e)};
    // Scene pieces the role needs: found now, created after the snapshot so undo removes them.
    Entity rules;
    w.each<script::GameRulesComponent>([&](Entity r, script::GameRulesComponent&) { rules = r; });
    Entity camera, follow;
    w.each<CameraComponent>([&](Entity c, CameraComponent& cc) {
        if (camera.is_null() || cc.primary) camera = c;
    });
    w.each<script::CameraFollowComponent>([&](Entity c, script::CameraFollowComponent&) { follow = c; });
    const bool wants_rules = role != Role::Hazard && role != Role::Checkpoint;
    const bool wants_follow = role == Role::Player && follow.is_null() && !camera.is_null() && camera != e;
    if (wants_follow) touched.push_back(s.uuid(camera));
    auto before = capture(s, touched);

    const RoleInfo* info = nullptr;
    for (const auto& i : kRoles)
        if (i.role == role) info = &i;
    if (auto* n = w.get<Name>(e); n && info && default_name(n->value)) n->value = info->name;

    bool changed = false;
    switch (role) {
    case Role::Player: {
        if (!w.has<script::PlatformerMovementComponent>(e) && !w.has<script::TopDownMovementComponent>(e)) {
            if (two_d) w.add<script::PlatformerMovementComponent>(e);
            else w.add<script::TopDownMovementComponent>(e);
            changed = true;
        }
        if (!w.has<script::HealthComponent>(e)) {
            w.add<script::HealthComponent>(e).on_death = script::DeathAction::Respawn;
            changed = true;
        }
        if (wants_follow) {
            auto& f = w.add<script::CameraFollowComponent>(camera);
            const math::Vec3 cam = w.get<Transform>(camera)->position, at = w.get<Transform>(e)->position;
            f.target = w.get<Name>(e)->value;
            f.offset = cam - at; // keep the current framing
            if (two_d) {
                f.look_at = false;
                f.follow_y = false; // side view: the camera tracks left and right
            }
            changed = true;
        }
        break;
    }
    case Role::Enemy: {
        if (!w.has<script::HazardComponent>(e)) {
            w.add<script::HazardComponent>(e).stompable = true;
            changed = true;
        }
        if (!w.has<script::PatrolComponent>(e)) {
            auto& p = w.add<script::PatrolComponent>(e);
            p.offset = two_d ? math::Vec3{3, 0, 0} : math::Vec3{4, 0, 0};
            p.turn_at_ledges = true;
            changed = true;
        }
        if (!w.has<script::HealthComponent>(e)) {
            auto& h = w.add<script::HealthComponent>(e);
            h.max = h.current = 1;
            changed = true;
        }
        break;
    }
    case Role::Coin:
        if (!w.has<script::CollectibleComponent>(e)) {
            w.add<script::CollectibleComponent>(e);
            changed = true;
        }
        if (!two_d && !w.has<gameplay::RotatorComponent>(e)) {
            w.add<gameplay::RotatorComponent>(e).degrees_per_second = 120;
            changed = true;
        }
        break;
    case Role::Hazard:
        if (!w.has<script::HazardComponent>(e)) {
            w.add<script::HazardComponent>(e);
            changed = true;
        }
        break;
    case Role::Checkpoint:
        if (!w.has<script::CheckpointComponent>(e)) {
            w.add<script::CheckpointComponent>(e);
            changed = true;
        }
        break;
    case Role::Goal:
        if (!w.has<script::GoalComponent>(e)) {
            w.add<script::GoalComponent>(e);
            changed = true;
        }
        break;
    }
    if (wants_rules && rules.is_null()) {
        rules = s.create("Game Rules");
        w.add<script::GameRulesComponent>(rules);
        before.push_back(EntityState{s.uuid(rules), std::nullopt});
        changed = true;
    }
    if (!changed) {
        notify(std::string("It is already a ") + role_label(role));
        return false;
    }
    record_edit(std::string("Make ") + role_label(role), std::move(before));
    complete_guide_step(GuideStep::AddBehaviour);
    show_panel(Panel::Inspector);
    notify(std::string("Made it a ") + role_label(role) + ". Press Play to try it");
    return true;
}

void Editor::draw_roles(Entity) {
    if (play_state_ != PlayState::Editing || selection_.size() > 1) return;
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    ImGui::TextColored(tk::kTextMuted, "Make it a...");
    kit::tooltip("One click adds the Behaviours for that role. Fine-tune them below.");
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImGuiStyle& st = ImGui::GetStyle();
    float x = 0;
    for (const auto& r : kRoles) {
        const float bw = ImGui::CalcTextSize(r.label).x + st.FramePadding.x * 2;
        if (x > 0 && x + st.ItemSpacing.x + bw <= avail) ImGui::SameLine();
        else x = 0;
        x += (x > 0 ? st.ItemSpacing.x : 0) + bw;
        ImGui::PushID(r.label);
        if (ImGui::Button(r.label)) pending_role_ = r.role;
        ImGui::PopID();
        kit::tooltip(r.tooltip);
    }
}

} // namespace victus::editor
