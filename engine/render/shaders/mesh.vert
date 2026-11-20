#version 450
// SPDX-License-Identifier: Apache-2.0
// Forward/PBR vertex stage, always instanced: per-instance model and normal matrices come from
// vertex buffer slot 1 (a single object is a batch of one). gl_Position is invariant so the depth
// pre-pass (depth.vert) and this pass produce bit-identical depth (EQUAL/LEQUAL without z-fighting).
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
layout(location = 13) in vec4 in_color; // RGBA8: sRGB rgb, linear alpha
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
} frame;
layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;
layout(location = 4) out vec4 v_color;
layout(location = 5) out vec3 v_lightmap; // not lightmapped (see mesh_lightmap.vert)
invariant gl_Position;
void main() {
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    mat3 normal_matrix = mat3(in_normal0.xyz, in_normal1.xyz, in_normal2.xyz);
    vec4 world = model * vec4(in_pos, 1.0);
    v_world_pos = world.xyz;
    v_normal = normalize(normal_matrix * in_normal);
    v_tangent = vec4(normalize(mat3(model) * in_tangent.xyz), in_tangent.w);
    v_uv = in_uv;
    // Exact sRGB decode (vertex colours are stored sRGB-encoded for precision).
    vec3 c = in_color.rgb;
    v_color = vec4(mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)), in_color.a);
    gl_Position = frame.view_proj * world;
    v_lightmap = vec3(0.0);
}
