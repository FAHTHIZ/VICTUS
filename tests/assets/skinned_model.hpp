// SPDX-License-Identifier: Apache-2.0
// Test-only skinned model: a two-segment column (y 0..1 bound to joint 0 "Hip", y 1..2 bound to
// joint 1 "Spine" at y = 1) with clips "Bend" (Spine rotates +90 degrees about Z over 1 s, so the
// upper segment points to -X) and "Lift" (Hip rises 1 m over 2 s, step keys at 0/1/2 s).
#pragma once
#include "victus/assets/cooked.hpp"

#include <numbers>

namespace victus::test {

inline assets::CookedModel make_skinned_column(bool unlit = false) {
    using namespace victus::math;
    assets::CookedModel m;
    assets::ModelMaterial mat;
    mat.name = "Column";
    mat.base_color = {0.9f, 0.1f, 0.05f, 1};
    mat.metallic = 0;
    mat.roughness = 0.6f;
    mat.unlit = unlit;
    m.materials.push_back(mat);

    render::MeshData mesh;
    const render::MeshData seg = render::make_cylinder(0.25f, 1.0f, 12);
    mesh.append(seg, Mat4::translation({0, 0.5f, 0}));
    const std::size_t bottom = mesh.vertices.size();
    mesh.append(seg, Mat4::translation({0, 1.5f, 0}));
    mesh.skin.resize(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) mesh.skin[i].joints[0] = i < bottom ? 0 : 1;
    m.meshes.push_back({"Column#0", std::move(mesh), 0});

    assets::ModelNode body;
    body.name = "Column";
    body.meshes = {0};
    body.skin = 0;
    assets::ModelNode hip;
    hip.name = "Hip";
    assets::ModelNode spine;
    spine.name = "Spine";
    spine.parent = 1;
    spine.translation = {0, 1, 0};
    m.nodes = {body, hip, spine};
    m.skins.push_back({"Skeleton", {1, 2}, {Mat4::identity(), Mat4::translation({0, -1, 0})}});

    animation::Clip bend{.name = "Bend", .duration = 1.0f};
    animation::Channel rot{.node = 2, .path = animation::Path::Rotation, .interpolation = animation::Interpolation::Linear, .times = {0, 1}};
    const Quat q = Quat::axis_angle({0, 0, 1}, std::numbers::pi_v<float> / 2);
    rot.values = {0, 0, 0, 1, q.x, q.y, q.z, q.w};
    bend.channels.push_back(rot);
    animation::Clip lift{.name = "Lift", .duration = 2.0f};
    animation::Channel tr{.node = 1, .path = animation::Path::Translation, .interpolation = animation::Interpolation::Step, .times = {0, 1, 2}};
    tr.values = {0, 0, 0, 0, 0.5f, 0, 0, 1, 0};
    lift.channels.push_back(tr);
    m.animations = {bend, lift};
    return m;
}

} // namespace victus::test
