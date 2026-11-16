// SPDX-License-Identifier: Apache-2.0
// Built-in project templates. Each produces a scene that runs immediately (Play) with the
// built-in behaviours, so a new project shows motion, lighting and a camera on first launch.
#include "victus/gameplay/gameplay.hpp"
#include "victus/project/project.hpp"

#include <algorithm>
#include <cmath>

namespace victus::project {

using namespace scene;
using gameplay::OscillatorComponent;
using gameplay::RotatorComponent;

namespace {

math::Quat look_rotation(math::Vec3 dir) {
    dir = math::normalize(dir);
    const float yaw = std::atan2(-dir.x, -dir.z);
    const float pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    return math::Quat::euler(pitch, yaw, 0.0f);
}

Entity add(Scene& s, std::string_view name, math::Vec3 pos, Entity parent = kNullEntity) {
    const Entity e = s.create(name, parent);
    s.world().get<Transform>(e)->position = pos;
    return e;
}

MeshRendererComponent& mesh(Scene& s, Entity e, std::string_view primitive, math::Color color, float metallic,
                            float roughness) {
    auto& m = s.world().add<MeshRendererComponent>(e);
    m.primitive = std::string(primitive);
    m.base_color = color;
    m.metallic = metallic;
    m.roughness = roughness;
    return m;
}

void camera_3d(Scene& s, math::Vec3 eye, math::Vec3 target) {
    const Entity cam = add(s, "Main Camera", eye);
    s.world().get<Transform>(cam)->rotation = look_rotation(target - eye);
    s.world().add<CameraComponent>(cam);
}

void sun(Scene& s) {
    const Entity e = add(s, "Sun", {0, 10, 0});
    s.world().get<Transform>(e)->rotation = look_rotation({-0.4f, -1.0f, -0.3f});
    auto& l = s.world().add<LightComponent>(e);
    l.type = LightComponent::Type::Directional;
    l.color = {1.0f, 0.96f, 0.9f, 1};
    l.intensity = 3.0f;
}

void build_3d(Scene& s) {
    camera_3d(s, {0, 4.5f, 9}, {0, 0.6f, 0});
    sun(s);
    const Entity ground = add(s, "Ground", {0, 0, 0});
    s.world().get<Transform>(ground)->scale = {20, 1, 20};
    mesh(s, ground, "plane", {0.16f, 0.17f, 0.19f, 1}, 0.0f, 0.9f);

    const Entity cube = add(s, "Spinning Cube", {-2.2f, 0.75f, 0});
    s.world().get<Transform>(cube)->scale = {1.5f, 1.5f, 1.5f};
    mesh(s, cube, "cube", {0.9f, 0.32f, 0.08f, 1}, 0.0f, 0.45f);
    s.world().add<RotatorComponent>(cube) = {{0, 1, 0}, 40.0f};

    const Entity sphere = add(s, "Floating Sphere", {0, 1.2f, 0.5f});
    s.world().get<Transform>(sphere)->scale = {1.4f, 1.4f, 1.4f};
    mesh(s, sphere, "sphere", {0.1f, 0.35f, 0.9f, 1}, 0.0f, 0.25f);
    s.world().add<OscillatorComponent>(sphere) = {{0, 0.4f, 0}, 0.4f, 0.0f, {}, false};

    const Entity pillar = add(s, "Metal Pillar", {2.4f, 1.0f, -0.4f});
    s.world().get<Transform>(pillar)->scale = {1.0f, 2.0f, 1.0f};
    mesh(s, pillar, "cylinder", {0.95f, 0.93f, 0.88f, 1}, 1.0f, 0.3f);

    const Entity lamp = add(s, "Warm Lamp", {0.5f, 2.2f, 2.5f});
    auto& l = s.world().add<LightComponent>(lamp);
    l.type = LightComponent::Type::Point;
    l.color = {1.0f, 0.62f, 0.3f, 1};
    l.intensity = 12.0f;
    l.range = 7.0f;
}

SpriteComponent& sprite(Scene& s, Entity e, math::Vec2 size, math::Color tint, std::int32_t layer) {
    auto& sp = s.world().add<SpriteComponent>(e);
    sp.size = size;
    sp.tint = tint;
    sp.layer = layer;
    return sp;
}

void build_2d(Scene& s) {
    const Entity cam = add(s, "Main Camera", {0, 0, 10});
    auto& c = s.world().add<CameraComponent>(cam);
    c.projection = CameraComponent::Projection::Orthographic;
    c.ortho_height = 10.0f;

    sprite(s, add(s, "Sky", {0, 1, -1}), {40, 14}, {0.38f, 0.6f, 0.9f, 1}, -10);
    sprite(s, add(s, "Sun Disc", {5.5f, 3.2f, -0.5f}), {1.6f, 1.6f}, {1.0f, 0.86f, 0.35f, 1}, -9);
    sprite(s, add(s, "Ground", {0, -4.2f, 0}), {40, 2.4f}, {0.22f, 0.55f, 0.27f, 1}, 0);
    sprite(s, add(s, "Platform", {2.5f, -1.0f, 0}), {3.0f, 0.4f}, {0.45f, 0.3f, 0.2f, 1}, 0);
    sprite(s, add(s, "Platform High", {-1.5f, 1.0f, 0}), {2.5f, 0.4f}, {0.45f, 0.3f, 0.2f, 1}, 0);

    const Entity player = add(s, "Player", {-4.0f, -2.5f, 0});
    sprite(s, player, {0.9f, 0.9f}, {0.95f, 0.45f, 0.15f, 1}, 2);
    s.world().add<OscillatorComponent>(player) = {{0, 0.9f, 0}, 0.8f, 0.0f, {}, false};

    const Entity spinner = add(s, "Spinner", {4.5f, 0.2f, 0});
    sprite(s, spinner, {1.2f, 1.2f}, {0.95f, 0.85f, 0.2f, 1}, 1);
    s.world().add<RotatorComponent>(spinner) = {{0, 0, 1}, 90.0f};
}

void build_simulation(Scene& s) {
    camera_3d(s, {0, 14, 18}, {0, 0, 0});
    sun(s);
    const Entity ground = add(s, "Ground", {0, -0.6f, 0});
    s.world().get<Transform>(ground)->scale = {40, 1, 40};
    mesh(s, ground, "plane", {0.3f, 0.32f, 0.35f, 1}, 0.0f, 0.95f);
    const Entity field = add(s, "Wave Field", {0, 0, 0});
    constexpr int kSide = 24;
    for (int z = 0; z < kSide; ++z) {
        for (int x = 0; x < kSide; ++x) {
            const float px = (static_cast<float>(x) - kSide * 0.5f) * 0.8f;
            const float pz = (static_cast<float>(z) - kSide * 0.5f) * 0.8f;
            const Entity cell = add(s, "Cell", {px, 0, pz}, field);
            s.world().get<Transform>(cell)->scale = {0.6f, 0.6f, 0.6f};
            const float d = std::sqrt(px * px + pz * pz);
            const float hue = 0.5f + 0.5f * std::sin(d * 0.4f);
            mesh(s, cell, "cube", {0.1f + 0.8f * hue, 0.35f, 1.0f - 0.8f * hue, 1}, 0.0f, 0.5f);
            s.world().add<OscillatorComponent>(cell) = {{0, 0.6f, 0}, 0.35f, -d * 0.6f, {}, false};
        }
    }
}

} // namespace

const std::vector<TemplateInfo>& templates() {
    static const std::vector<TemplateInfo> list = {
        {"3d", "3D Scene", "Perspective camera, sun with shadows, PBR primitives, a point light and animated objects."},
        {"2d", "2D Game", "Orthographic camera, layered sprites, platforms and animated characters."},
        {"simulation", "Simulation",
         "576 animated entities driven by systems: a starting point for large-scale simulation work."},
    };
    return list;
}

Status build_template_scene(std::string_view id, Scene& s) {
    s.clear();
    if (id == "3d") build_3d(s);
    else if (id == "2d") build_2d(s);
    else if (id == "simulation") build_simulation(s);
    else return make_error(ErrorCode::InvalidArgument, "unknown template '" + std::string(id) + "'");
    s.update_transforms();
    return {};
}

} // namespace victus::project
