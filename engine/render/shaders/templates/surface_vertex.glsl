#version 450
// SPDX-License-Identifier: Apache-2.0
// Vertex stage for project surface shaders that define a vertex function (ADR-0170):
//
//   void vertex(VertexInput i, inout Vertex v) { ... }
//
// runs after the model (and skin) transform and before projection, so it can move vertices in
// world space (wind, waves, wobble, vertex-animation offsets) and change normals, uvs and colours.
// One template, compiled at run time with:
//   VX_SKINNED   - joint palette skinning (as mesh_skinned.vert)
//   VX_LIGHTMAP  - lightmap atlas uv (as mesh_lightmap.vert)
//   VX_SHADOW    - shadow-map depth (sun cascades, local lights, rain occlusion): same
//                  displacement, output only the uv for alpha-tested casters
// The pipeline uses the same vertex inputs as the built-in forward variants. Uniform slot 0 is
// the pass's view-projection (camera or light), slot 1 the per-material VertexMaterial block.
// This file is not one of the precompiled built-in shaders (tools/shaders/compile_shaders.py only
// reads engine/render/shaders/*.vert|frag|comp).
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
#ifdef VX_SKINNED
layout(location = 11) in uvec4 in_joints;
layout(location = 12) in vec4 in_weights;
#if defined(VICTUS_ESSL)
layout(set = 0, binding = 15) uniform highp usampler2D vx_storage0;
vec4 joint_column(uint t) { return uintBitsToFloat(texelFetch(vx_storage0, ivec2(int(t & 1023u), int(t >> 10u)), 0)); }
mat4 joint_matrix(uint i) {
    uint t = i * 4u;
    return mat4(joint_column(t), joint_column(t + 1u), joint_column(t + 2u), joint_column(t + 3u));
}
#define JOINT(i) joint_matrix(i)
#else
// Metal buffer indices are renumbered after the uniform buffers by the run-time compiler.
// Surface shader textures (ADR-0171) take vertex sampler bindings 0..VX_CUSTOM_TEXTURES-1.
layout(std430, set = 0, binding = VX_CUSTOM_TEXTURES) readonly buffer Joints { mat4 joints[]; } palette;
#define JOINT(i) palette.joints[i]
#endif
#endif
#if defined(VX_LIGHTMAP) && !defined(VX_SHADOW)
layout(location = 14) in vec2 in_lightmap_uv;
#endif
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
} frame;
layout(set = 1, binding = 1) uniform VertexMaterial {
    vec4 time;      // x: seconds (game time while playing), y: 1 in shadow passes
    vec4 params[4]; // the material's shader_params, as i.params in surface()
} vx_material;
#ifdef VX_SHADOW
layout(location = 0) out vec2 v_uv;
#else
layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;
layout(location = 4) out vec4 v_color;
layout(location = 5) out vec3 v_lightmap;
#endif

// What the vertex function reads.
struct VertexInput {
    vec3 position;      // object space (after skinning)
    vec3 normal;        // object space (after skinning)
    vec2 uv;            // mesh uv
    vec4 color;         // linear vertex colour
    vec3 object_origin; // world position of the object's origin (per-object phase offsets)
    float time;         // seconds
    bool shadow;        // true while drawing into a shadow map
    vec4 params[4];     // material shader_params
};
// What the vertex function may change (world space).
struct Vertex {
    vec3 world_position;
    vec3 normal;
    vec2 uv;
    vec4 color;
};
#define VICTUS_VERTEX_STAGE 1
// VICTUS_VERTEX_CODE

void main() {
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    mat3 normal_matrix = mat3(in_normal0.xyz, in_normal1.xyz, in_normal2.xyz);
    vec4 local = vec4(in_pos, 1.0);
    vec3 local_normal = in_normal;
    vec3 local_tangent = in_tangent.xyz;
#ifdef VX_SKINNED
    uint base = uint(in_normal0.w + 0.5);
    mat4 skin = in_weights.x * JOINT(base + in_joints.x) + in_weights.y * JOINT(base + in_joints.y) +
                in_weights.z * JOINT(base + in_joints.z) + in_weights.w * JOINT(base + in_joints.w);
    local = skin * local;
    local_normal = mat3(skin) * local_normal;
    local_tangent = mat3(skin) * local_tangent;
#endif
    vec3 c = in_color.rgb;
    vec4 color = vec4(mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)), in_color.a);
    VertexInput vi;
    vi.position = local.xyz;
    vi.normal = local_normal;
    vi.uv = in_uv;
    vi.color = color;
    vi.object_origin = in_model3.xyz;
    vi.time = vx_material.time.x;
    vi.shadow = vx_material.time.y > 0.5;
    vi.params = vx_material.params;
    Vertex v;
    v.world_position = (model * local).xyz;
    v.normal = normalize(normal_matrix * local_normal);
    v.uv = in_uv;
    v.color = color;
    vertex(vi, v);
    gl_Position = frame.view_proj * vec4(v.world_position, 1.0);
#ifdef VX_SHADOW
    v_uv = v.uv;
#else
    v_world_pos = v.world_position;
    v_normal = normalize(v.normal);
    v_tangent = vec4(normalize(mat3(model) * local_tangent), in_tangent.w);
    v_uv = v.uv;
    v_color = v.color;
#ifdef VX_LIGHTMAP
    v_lightmap = vec3(in_lightmap_uv + vec2(in_normal1.w, in_normal2.w), 1.0);
#else
    v_lightmap = vec3(0.0);
#endif
#endif
}
