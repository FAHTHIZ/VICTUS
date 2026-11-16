#version 450
// SPDX-License-Identifier: Apache-2.0
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    mat4 light_view_proj;
} frame;
layout(set = 1, binding = 1) uniform Object {
    mat4 model;
    mat4 normal_matrix;
} object;
layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;
layout(location = 4) out vec4 v_shadow_pos;
void main() {
    vec4 world = object.model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    v_normal = normalize(mat3(object.normal_matrix) * in_normal);
    v_tangent = vec4(normalize(mat3(object.model) * in_tangent.xyz), in_tangent.w);
    v_uv = in_uv;
    v_shadow_pos = frame.light_view_proj * world;
    gl_Position = frame.view_proj * world;
}
