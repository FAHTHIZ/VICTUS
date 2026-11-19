// SPDX-License-Identifier: Apache-2.0
// Inspector > "Make it a...": one click turns the selected object into a player, enemy, coin,
// hazard, checkpoint, goal, moving platform, key, door, sign, switch or jump pad by adding the Behaviours that role needs (ADR-0120), plus the scene
// pieces it depends on (Game Rules, a camera that follows the player). One undo step.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/gameplay/gameplay.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>

#include <array>
#include <cmath>

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

constexpr std::array<RoleInfo, 12> kRoles = {{
    {Role::Player, "Player", "Player", "Moves with the keyboard or a gamepad, has health and respawns. The camera follows it."},
    {Role::Enemy, "Enemy", "Enemy", "Walks back and forth and hurts the player; jump on it to defeat it."},
    {Role::Coin, "Coin", "Coin", "Adds 1 to the score when the player touches it."},
    {Role::Hazard, "Hazard", "Spikes", "Hurts the player on touch, like spikes or lava."},
    {Role::Checkpoint, "Checkpoint", "Checkpoint", "The player respawns here after reaching it."},
    {Role::Goal, "Goal", "Goal", "Reaching it wins the game."},
    {Role::MovingPlatform, "Moving Platform", "Platform",
     "Travels back and forth; whoever stands on it rides along. Set how far in Moving Platform > Offset."},
    {Role::Key, "Key", "Key", "The player picks it up and carries it; a Door needs one to open."},
    {Role::Door, "Door", "Door", "A solid door that opens when the player touches it carrying a Key."},
    {Role::Sign, "Sign", "Sign", "Shows a message while the player stands next to it. Edit the text in Trigger > Message."},
    {Role::Switch, "Switch", "Switch",
     "The player presses E next to it to start the Moving Platform named in Trigger > Target (\"Platform\")."},
    {Role::JumpPad, "Jump Pad", "Jump Pad", "A spring: the player bounces high when landing on it."},
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
    const bool wants_rules = role != Role::Hazard && role != Role::Checkpoint && role != Role::MovingPlatform &&
                            role != Role::Key && role != Role::Door && role != Role::Sign &&
                            role != Role::JumpPad && role != Role::Switch;
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
            if (two_d) {
                f.offset = {0, cam.y - at.y, cam.z - at.z}; // centred on the player, same depth
                f.look_at = false;
                f.follow_y = false; // side view: the camera tracks left and right
            } else {
                f.offset = {0, 4, 8}; // behind and above, looking at the player
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
    case Role::MovingPlatform:
        if (!w.has<script::MovingPlatformComponent>(e)) {
            w.add<script::MovingPlatformComponent>(e).wait = 1.0f;
            changed = true;
        }
        break;
    case Role::Key:
        if (!w.has<script::CollectibleComponent>(e)) {
            auto& c = w.add<script::CollectibleComponent>(e);
            c.kind = script::CollectibleKind::Item;
            c.item = "Key";
            changed = true;
        }
        break;
    case Role::Door:
        if (!w.has<script::TriggerComponent>(e)) {
            auto& t = w.add<script::TriggerComponent>(e);
            t.required_item = "Key";
            t.action = script::TriggerAction::Remove;
            t.message = "The door opens";
            changed = true;
        }
        // Solid, so the player stops at it until it opens.
        if (two_d && !w.has<physics::Collider2DComponent>(e)) {
            physics::Collider2DComponent c;
            if (const auto* sp = w.get<SpriteComponent>(e)) c.size = sp->size;
            w.add<physics::Collider2DComponent>(e, c);
            if (!w.has<physics::RigidBody2DComponent>(e)) w.add<physics::RigidBody2DComponent>(e).type = physics::BodyType::Static;
            changed = true;
        } else if (!two_d && !w.has<physics::ColliderComponent>(e)) {
            w.add<physics::ColliderComponent>(e);
            changed = true;
        }
        break;
    case Role::JumpPad:
        if (!w.has<script::JumpPadComponent>(e)) {
            w.add<script::JumpPadComponent>(e);
            changed = true;
        }
        break;
    case Role::Switch:
        if (!w.has<script::TriggerComponent>(e)) {
            auto& t = w.add<script::TriggerComponent>(e);
            t.press_to_use = true;
            t.action = script::TriggerAction::Start;
            t.target = "Platform";
            t.message = "Click";
            t.message_time = 1.5f;
            changed = true;
        }
        break;
    case Role::Sign:
        if (!w.has<script::TriggerComponent>(e)) {
            auto& t = w.add<script::TriggerComponent>(e);
            t.message = "Hello! Change this text in the Inspector (Trigger > Message).";
            t.message_time = 0;
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

std::vector<physics::DebugLine> Editor::behaviour_guides(Entity e, bool flat) {
    std::vector<physics::DebugLine> out;
    Scene& s = active_scene();
    if (e.is_null() || !s.world().alive(e)) return out;
    auto& w = s.world();
    const math::Mat4 m = s.world_matrix(e);
    const math::Vec3 p = m.transform_point({});
    const bool playing = play_state_ != PlayState::Editing;
    const math::Color path{0.3f, 0.85f, 1.0f, 1}, ghost{0.3f, 0.85f, 1.0f, 0.55f};
    const auto cross = [&](math::Vec3 c, float r) {
        const math::Vec3 u = flat ? math::Vec3{0, r, 0} : math::Vec3{0, 0, r};
        out.push_back({c - math::Vec3{r, 0, 0}, c + math::Vec3{r, 0, 0}, path});
        out.push_back({c - u, c + u, path});
    };
    const auto circle = [&](math::Vec3 c, float r, math::Color col) {
        constexpr int kSegments = 48;
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = 6.2831853f * static_cast<float>(i) / kSegments, a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
            const auto at = [&](float a) {
                return flat ? c + math::Vec3{r * std::cos(a), r * std::sin(a), 0} : c + math::Vec3{r * std::cos(a), 0, r * std::sin(a)};
            };
            out.push_back({at(a0), at(a1), col});
        }
    };
    if (const auto* pa = w.get<script::PatrolComponent>(e); pa && math::length(pa->offset) > 1e-4f) {
        const math::Vec3 a = playing && pa->has_origin ? pa->origin : p;
        out.push_back({a, a + pa->offset, path});
        cross(a, 0.15f);
        cross(a + pa->offset, 0.15f);
    }
    if (const auto* mp = w.get<script::MovingPlatformComponent>(e); mp && math::length(mp->offset) > 1e-4f) {
        const float t = mp->progress, u = mp->smooth ? t * t * (3 - 2 * t) : t;
        const math::Vec3 a = playing && mp->has_origin ? p - mp->offset * u : p;
        out.push_back({a, a + mp->offset, path});
        cross(a, 0.15f);
        // Outline of the platform at the far end.
        const math::Aabb local = entity_bounds(e, nullptr);
        math::Vec3 c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = m.transform_point({(i & 1) ? local.max.x : local.min.x, (i & 2) ? local.max.y : local.min.y,
                                      (i & 4) ? local.max.z : local.min.z}) +
                   (a + mp->offset - p);
        static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                              {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (int k = 0; k < (flat ? 4 : 12); ++k) {
            // Flat: the four edges of the front face (corners 0,1,3,2).
            static constexpr int kFace[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
            const int i0 = flat ? kFace[k][0] : kEdges[k][0], i1 = flat ? kFace[k][1] : kEdges[k][1];
            out.push_back({c[i0], c[i1], ghost});
        }
    }
    if (const auto* jp = w.get<script::JumpPadComponent>(e); jp && jp->strength > 0) {
        // The launch arc of whatever it throws, using the first player's gravity scale.
        float scale = 1;
        w.each<script::PlatformerMovementComponent>([&](Entity pe, script::PlatformerMovementComponent&) {
            if (const auto* c2 = w.get<physics::CharacterController2DComponent>(pe)) scale = c2->gravity_scale;
            else if (const auto* c3 = w.get<physics::CharacterControllerComponent>(pe)) scale = c3->gravity_scale;
        });
        const float g = 9.81f * std::max(scale, 0.05f);
        const math::Vec3 dir = math::length(jp->direction) > 1e-4f ? math::normalize(jp->direction) : math::Vec3{0, 1, 0};
        const math::Vec3 v = dir * jp->strength;
        const float top = entity_bounds(e, nullptr).max.y * m.transform_vector({0, 1, 0}).y;
        const math::Vec3 start{p.x, p.y + top, p.z};
        const float t_end = std::max(v.y, 0.0f) / g * 2.0f; // back to the launch height
        constexpr int kSteps = 32;
        math::Vec3 prev = start;
        for (int i = 1; i <= kSteps; ++i) {
            const float t = t_end * static_cast<float>(i) / kSteps;
            const math::Vec3 at = start + v * t - math::Vec3{0, 0.5f * g * t * t, 0};
            out.push_back({prev, at, i <= kSteps / 2 ? path : ghost});
            prev = at;
        }
        cross(start + math::Vec3{v.x, 0, v.z} * (t_end * 0.5f) + math::Vec3{0, v.y * v.y / (2 * g), 0}, 0.2f); // the peak
    }
    if (const auto* ch = w.get<script::ChaseComponent>(e)) {
        if (ch->detect_radius > 0) circle(p, ch->detect_radius, {1.0f, 0.4f, 0.3f, 0.9f});
        if (ch->lose_radius > ch->detect_radius) circle(p, ch->lose_radius, {1.0f, 0.4f, 0.3f, 0.35f});
    }
    return out;
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
