// SPDX-License-Identifier: Apache-2.0
// Script bindings: physics.* and entity physics, character controller and joint methods.
#include "script_internal.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace victus::script::detail {


// ---- physics ----

physics::PhysicsWorld* physics_of(lua_State* L) { return physics::world_of(scene_of(L)); }


physics::PhysicsWorld& check_physics(lua_State* L) {
    auto* w = physics_of(L);
    if (!w) luaL_error(L, "physics is not running for this scene");
    return *w;
}


std::uint32_t opt_mask(lua_State* L, int idx) {
    return static_cast<std::uint32_t>(luaL_optinteger(L, idx, physics::kAllLayers));
}


void push_hit(lua_State* L, const std::optional<physics::RaycastHit>& hit) {
    if (!hit) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 4);
    push_entity(L, hit->entity);
    lua_setfield(L, -2, "entity");
    push_vec3(L, hit->point);
    lua_setfield(L, -2, "point");
    push_vec3(L, hit->normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit->distance);
    lua_setfield(L, -2, "distance");
}


void push_entities(lua_State* L, const std::vector<Entity>& list) {
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        push_entity(L, list[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}


// physics.raycast(origin, direction, max_distance = 1000, mask = all, ignore = nil)
int physics_raycast(lua_State* L) {
    auto& w = check_physics(L);
    push_hit(L, w.raycast(check_vec3(L, 1), check_vec3(L, 2), static_cast<float>(luaL_optnumber(L, 3, 1000.0)),
                          opt_mask(L, 4), opt_entity(L, 5)));
    return 1;
}

// physics.raycast2d(origin, direction, ...) - x/y of the vectors are used.
int physics_raycast2d(lua_State* L) {
    auto& w = check_physics(L);
    const math::Vec3 o = check_vec3(L, 1), d = check_vec3(L, 2);
    push_hit(L, w.raycast_2d({o.x, o.y}, {d.x, d.y}, static_cast<float>(luaL_optnumber(L, 3, 1000.0)), opt_mask(L, 4),
                             opt_entity(L, 5)));
    return 1;
}

int physics_overlap_sphere(lua_State* L) {
    auto& w = check_physics(L);
    push_entities(L, w.overlap_sphere(check_vec3(L, 1), static_cast<float>(luaL_checknumber(L, 2)), opt_mask(L, 3)));
    return 1;
}

int physics_overlap_box(lua_State* L) {
    auto& w = check_physics(L);
    const math::Quat q = lua_isnoneornil(L, 3) ? math::Quat{} : from_euler_degrees(check_vec3(L, 3));
    push_entities(L, w.overlap_box(check_vec3(L, 1), check_vec3(L, 2), q, opt_mask(L, 4)));
    return 1;
}

int physics_overlap_circle(lua_State* L) {
    auto& w = check_physics(L);
    const math::Vec3 c = check_vec3(L, 1);
    push_entities(L, w.overlap_circle_2d({c.x, c.y}, static_cast<float>(luaL_checknumber(L, 2)), opt_mask(L, 3)));
    return 1;
}

int physics_overlap_box2d(lua_State* L) {
    auto& w = check_physics(L);
    const math::Vec3 c = check_vec3(L, 1), h = check_vec3(L, 2);
    const float angle = math::radians(static_cast<float>(luaL_optnumber(L, 3, 0.0)));
    push_entities(L, w.overlap_box_2d({c.x, c.y}, {h.x, h.y}, angle, opt_mask(L, 4)));
    return 1;
}

int physics_gravity(lua_State* L) {
    push_vec3(L, check_physics(L).gravity());
    return 1;
}

int physics_set_gravity(lua_State* L) {
    check_physics(L).set_gravity(check_vec3(L, 1));
    return 0;
}

int physics_gravity2d(lua_State* L) {
    const math::Vec2 g = check_physics(L).gravity_2d();
    lua_pushvector(L, g.x, g.y, 0);
    return 1;
}

int physics_set_gravity2d(lua_State* L) {
    const math::Vec3 g = check_vec3(L, 1);
    check_physics(L).set_gravity_2d({g.x, g.y});
    return 0;
}

int physics_available(lua_State* L) {
    lua_pushboolean(L, physics_of(L) != nullptr);
    return 1;
}


// entity:move_character(velocity): desired walking velocity (persists until changed).
int entity_move_character(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (auto* w = physics::world_of(s)) w->move_character(e, check_vec3(L, 2));
    return 0;
}


// entity:jump(speed) -> bool (false when not grounded)
int entity_jump(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* w = physics::world_of(s);
    lua_pushboolean(L, w && w->jump_character(e, static_cast<float>(luaL_checknumber(L, 2))));
    return 1;
}


// entity:drop_through(seconds?) -> bool: a 2D character ignores one-way platforms for `seconds`
// (default 0.25) so it falls through the one it stands on. False for anything but a 2D character.
int entity_drop_through(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* w = physics::world_of(s);
    lua_pushboolean(L, w && w->drop_through(e, static_cast<float>(luaL_optnumber(L, 2, 0.25))));
    return 1;
}


// entity:character_state() -> {valid, grounded, on_steep_slope, ground_normal, velocity, ground}
int entity_character_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* w = physics::world_of(s);
    const physics::CharacterState st = w ? w->character_state(e) : physics::CharacterState{};
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushboolean(L, st.grounded);
    lua_setfield(L, -2, "grounded");
    lua_pushboolean(L, st.on_steep_slope);
    lua_setfield(L, -2, "on_steep_slope");
    push_vec3(L, st.ground_normal);
    lua_setfield(L, -2, "ground_normal");
    push_vec3(L, st.velocity);
    lua_setfield(L, -2, "velocity");
    if (!st.ground.is_null()) {
        push_entity(L, st.ground);
        lua_setfield(L, -2, "ground");
    }
    return 1;
}


// entity:joint_state() -> {valid, active, broken, angle, position, force}
int entity_joint_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* w = physics::world_of(s);
    const physics::JointState st = w ? w->joint_state(e) : physics::JointState{};
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushboolean(L, st.active);
    lua_setfield(L, -2, "active");
    lua_pushboolean(L, st.broken);
    lua_setfield(L, -2, "broken");
    lua_pushnumber(L, st.angle);
    lua_setfield(L, -2, "angle");
    lua_pushnumber(L, st.position);
    lua_setfield(L, -2, "position");
    lua_pushnumber(L, st.force);
    lua_setfield(L, -2, "force");
    return 1;
}


// entity:set_motor(enabled, speed): hinge/revolute/wheel (deg/s) or slider/prismatic (m/s) motor, 3D or 2D.
int entity_set_motor(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const bool enabled = lua_toboolean(L, 2) != 0;
    const float speed = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    if (auto* w = physics::world_of(s)) {
        w->set_joint_motor(s, e, enabled, speed);
    } else if (auto* j = s.world().get<physics::JointComponent>(e)) {
        j->motor = enabled;
        j->motor_speed = speed;
    } else if (auto* j2 = s.world().get<physics::Joint2DComponent>(e)) {
        j2->motor = enabled;
        j2->motor_speed = speed;
    }
    return 0;
}

namespace {
constexpr const char* kRagdollModes[] = {"off", "kinematic", "powered", "passive"};
} // namespace

// entity:set_ragdoll(mode [, blend]): "off" | "kinematic" | "powered" | "passive" on a Ragdoll root.
int entity_set_ragdoll(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string_view mode = luaL_checkstring(L, 2);
    int index = -1;
    for (int i = 0; i < 4; ++i)
        if (mode == kRagdollModes[i]) index = i;
    if (index < 0) luaL_error(L, "set_ragdoll: unknown mode '%s' (off, kinematic, powered, passive)", std::string(mode).c_str());
    auto* r = s.world().get<physics::RagdollComponent>(e);
    if (!r) luaL_error(L, "set_ragdoll: the entity has no Ragdoll component");
    r->mode = static_cast<physics::RagdollComponent::Mode>(index);
    if (lua_isnumber(L, 3)) r->blend = std::clamp(static_cast<float>(lua_tonumber(L, 3)), 0.0f, 1.0f);
    return 0;
}

// entity:ragdoll_state() -> {valid, mode, bodies, awake, center_of_mass, velocity, pose_error}
int entity_ragdoll_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* w = physics::world_of(s);
    const physics::RagdollState st = w ? w->ragdoll_state(e) : physics::RagdollState{};
    lua_createtable(L, 0, 7);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushstring(L, kRagdollModes[static_cast<int>(st.mode) & 3]);
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, static_cast<int>(st.bodies));
    lua_setfield(L, -2, "bodies");
    lua_pushboolean(L, st.awake);
    lua_setfield(L, -2, "awake");
    push_vec3(L, st.center_of_mass);
    lua_setfield(L, -2, "center_of_mass");
    push_vec3(L, st.velocity);
    lua_setfield(L, -2, "velocity");
    lua_pushnumber(L, st.pose_error);
    lua_setfield(L, -2, "pose_error");
    return 1;
}

// entity:damage(point, impulse): damages a Destructible (or the one owning a fragment) at a world
// point; it breaks, or frees more pieces, on the next physics step when impulse >= strength.
int entity_damage(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 point = check_vec3(L, 2);
    const float impulse = static_cast<float>(luaL_checknumber(L, 3));
    if (auto* w = physics::world_of(s)) w->damage(e, point, impulse);
    return 0;
}

// entity:destructible_state() -> {broken, fragments, loose} for a Destructible, or nil.
int entity_destructible_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* d = s.world().get<physics::DestructibleComponent>(e);
    if (!d) {
        lua_pushnil(L);
        return 1;
    }
    const Uuid id = s.uuid(e);
    int fragments = 0, loose = 0;
    s.world().each<physics::DestructibleFragmentComponent>([&](Entity f, physics::DestructibleFragmentComponent& c) {
        if (c.owner != id) return;
        ++fragments;
        if (const auto* rb = s.world().get<physics::RigidBodyComponent>(f); rb && rb->type == physics::BodyType::Dynamic) ++loose;
    });
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, d->broken);
    lua_setfield(L, -2, "broken");
    lua_pushinteger(L, fragments);
    lua_setfield(L, -2, "fragments");
    lua_pushinteger(L, loose);
    lua_setfield(L, -2, "loose");
    return 1;
}

// entity:ragdoll_root() -> the Ragdoll entity whose body this bone is, or nil.
int entity_ragdoll_root(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* w = physics::world_of(s);
    const Entity root = w ? w->ragdoll_of(e) : Entity{};
    if (root.is_null()) lua_pushnil(L);
    else push_entity(L, root);
    return 1;
}

} // namespace victus::script::detail
