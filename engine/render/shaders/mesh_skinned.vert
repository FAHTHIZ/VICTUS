#version 450
// SPDX-License-Identifier: Apache-2.0
// Skinned forward/PBR vertex stage. Same instancing as mesh.vert plus a per-vertex skin stream
// (slot 2: four joint indices + weights) and a joint palette in a vertex-stage storage buffer.
// Each instance's palette starts at in_normal0.w, so skinned instances of one mesh still batch.
// The skinning arithmetic is shared bit-for-bit with depth_skinned.vert (invariant position).
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec4 in_model0;
layout(location = 5) in vec4 in_model1;
layout(location = 6) in vec4 in_model2;
layout(location = 7) in vec4 in_model3;
layout(location = 8) in vec4 in_normal0;
layout(location = 9) in vec4 in_normal1;
layout(location = 10) in vec4 in_normal2;
layout(location = 11) in uvec4 in_joints;
layout(location = 12) in vec4 in_weights;
// SDL_GPU: SPIR-V vertex storage buffers live in set 0 after textures (binding 0 here); MSL
// buffers are uniform buffers first, then storage buffers ([[buffer(1)]] after one uniform).
#ifdef VICTUS_MSL
layout(std430, set = 0, binding = 1) readonly buffer Joints { mat4 joints[]; } palette;
#else
layout(std430, set = 0, binding = 0) readonly buffer Joints { mat4 joints[]; } palette;
#endif
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
} frame;
layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;
invariant gl_Position;
void main() {
    uint base = uint(in_normal0.w + 0.5);
    mat4 skin = in_weights.x * palette.joints[base + in_joints.x] + in_weights.y * palette.joints[base + in_joints.y] +
                in_weights.z * palette.joints[base + in_joints.z] + in_weights.w * palette.joints[base + in_joints.w];
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    mat3 normal_matrix = mat3(in_normal0.xyz, in_normal1.xyz, in_normal2.xyz);
    vec4 world = model * (skin * vec4(in_pos, 1.0));
    mat3 skin3 = mat3(skin);
    v_world_pos = world.xyz;
    v_normal = normalize(normal_matrix * (skin3 * in_normal));
    v_tangent = vec4(normalize(mat3(model) * (skin3 * in_tangent.xyz)), in_tangent.w);
    v_uv = in_uv;
    gl_Position = frame.view_proj * world;
}
