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
//                  displacement, output only the uv for alpha-tested casters; also the camera
//                  depth pre-pass (ADR-0244), where `shadow` is false
//   VX_MOTION    - object motion vectors (ADR-0244): the function runs twice, at this frame's
//                  time and model and at the previous frame's, and the pass writes how far the
//                  displaced vertex moved (motion.frag). Instances carry the previous model as
//                  three affine rows (locations 3, 14, 15) and, skinned, the previous palette
//                  base in in_normal1.w.
//   VX_RETAINED  - with VX_MOTION: retained group instances (ADR-0162), which never move, so the
//                  previous model is the current one and only time differs
// gl_Position is invariant and computed by the same expressions in every variant, so the depth
// pre-pass, the forward pass and the motion pass agree bit for bit.
// The pipeline uses the same vertex inputs as the built-in forward variants. Uniform slot 0 is
// the pass's view-projection (camera or light), slot 1 the per-material VertexMaterial block.
// This file is not one of the precompiled built-in shaders (tools/shaders/compile_shaders.py only
// reads engine/render/shaders/*.vert|frag|comp).
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
#if defined(VX_MOTION) && !defined(VX_RETAINED)
layout(location = 3) in vec4 in_prev_row0; // previous model matrix, affine rows
layout(location = 14) in vec4 in_prev_row1;
layout(location = 15) in vec4 in_prev_row2;
#else
layout(location = 3) in vec4 in_tangent;
#endif
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
// Skin matrix of up to four influences (ADR-0205). The palette's first influence picks the
// method: affine LBS matrices have column 3 w = 1; dual quaternion entries (render/skinning.hpp)
// store real, dual and scale in columns 0-2 and w = -1. The branch is uniform per draw.
// Kept identical in every skinned stage (invariant depth between passes).
mat4 victus_skin(uint base, uvec4 j, vec4 w) {
    mat4 m0 = JOINT(base + j.x);
    mat4 m1 = JOINT(base + j.y);
    mat4 m2 = JOINT(base + j.z);
    mat4 m3 = JOINT(base + j.w);
    if (m0[3].w > 0.0) return w.x * m0 + w.y * m1 + w.z * m2 + w.w * m3;
    vec4 q0 = m0[0];
    vec4 real = w.x * q0;
    vec4 dual = w.x * m0[1];
    vec3 scale = w.x * m0[2].xyz + w.y * m1[2].xyz + w.z * m2[2].xyz + w.w * m3[2].xyz;
    float s1 = dot(q0, m1[0]) < 0.0 ? -w.y : w.y;
    float s2 = dot(q0, m2[0]) < 0.0 ? -w.z : w.z;
    float s3 = dot(q0, m3[0]) < 0.0 ? -w.w : w.w;
    real += s1 * m1[0] + s2 * m2[0] + s3 * m3[0];
    dual += s1 * m1[1] + s2 * m2[1] + s3 * m3[1];
    float inv = inversesqrt(max(dot(real, real), 1e-20));
    real *= inv;
    dual *= inv;
    vec3 t = 2.0 * (real.w * dual.xyz - dual.w * real.xyz + cross(real.xyz, dual.xyz));
    float x2 = real.x + real.x, y2 = real.y + real.y, z2 = real.z + real.z;
    float xx = real.x * x2, yy = real.y * y2, zz = real.z * z2;
    float xy = real.x * y2, xz = real.x * z2, yz = real.y * z2;
    float wx = real.w * x2, wy = real.w * y2, wz = real.w * z2;
    return mat4(vec4((1.0 - (yy + zz)) * scale.x, (xy + wz) * scale.x, (xz - wy) * scale.x, 0.0),
                vec4((xy - wz) * scale.y, (1.0 - (xx + zz)) * scale.y, (yz + wx) * scale.y, 0.0),
                vec4((xz + wy) * scale.z, (yz - wx) * scale.z, (1.0 - (xx + yy)) * scale.z, 0.0),
                vec4(t, 1.0));
}
#endif
#if defined(VX_LIGHTMAP) && !defined(VX_SHADOW) && !defined(VX_MOTION)
layout(location = 14) in vec2 in_lightmap_uv;
#endif
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
#ifdef VX_MOTION
    mat4 prev_view_proj; // previous frame, unjittered
#endif
} frame;
layout(set = 1, binding = 1) uniform VertexMaterial {
    vec4 time;      // x: seconds (game time while playing), y: 1 in shadow passes, z: previous frame's seconds
    vec4 params[4]; // the material's shader_params, as i.params in surface()
} vx_material;
invariant gl_Position;
#if defined(VX_MOTION)
layout(location = 0) out vec4 v_current;  // previous-camera clip position of the current point
layout(location = 1) out vec4 v_previous; // ... of the same point last frame
#elif defined(VX_SHADOW)
layout(location = 0) out vec2 v_uv;
#else
layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;
layout(location = 4) out vec4 v_color;
layout(location = 5) out vec3 v_lightmap;
layout(location = 6) out vec4 v_object; // xyz: rest position in object metres, w: handedness (ADR-0246)
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

// The displaced world position (and, through `v`, the rest of the vertex) of one evaluation.
// Every variant positions vertices through this function, so the arithmetic is shared.
Vertex victus_run_vertex(mat4 model, mat3 normal_matrix, vec4 local, vec3 local_normal, vec4 color, float time) {
    VertexInput vi;
    vi.position = local.xyz;
    vi.normal = local_normal;
    vi.uv = in_uv;
    vi.color = color;
    vi.object_origin = model[3].xyz;
    vi.time = time;
    vi.shadow = vx_material.time.y > 0.5;
    vi.params = vx_material.params;
    Vertex v;
    v.world_position = (model * local).xyz;
    v.normal = normalize(normal_matrix * local_normal);
    v.uv = in_uv;
    v.color = color;
    vertex(vi, v);
    return v;
}

void main() {
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    mat3 normal_matrix = mat3(in_normal0.xyz, in_normal1.xyz, in_normal2.xyz);
    vec4 local = vec4(in_pos, 1.0);
    vec3 local_normal = in_normal;
#ifdef VX_SKINNED
    uint base = uint(in_normal0.w + 0.5);
    mat4 skin = victus_skin(base, in_joints, in_weights);
    local = skin * local;
    local_normal = mat3(skin) * local_normal;
#endif
    vec3 c = in_color.rgb;
    vec4 color = vec4(mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)), in_color.a);
    Vertex v = victus_run_vertex(model, normal_matrix, local, local_normal, color, vx_material.time.x);
    gl_Position = frame.view_proj * vec4(v.world_position, 1.0);
#if defined(VX_MOTION)
#ifdef VX_RETAINED
    mat4 prev_model = model;
    mat3 prev_normal_matrix = normal_matrix;
#else
    mat4 prev_model = transpose(mat4(in_prev_row0, in_prev_row1, in_prev_row2, vec4(0.0, 0.0, 0.0, 1.0)));
    // Inverse transpose of the previous basis: its cofactor columns over the determinant.
    vec3 pa = prev_model[0].xyz, pb = prev_model[1].xyz, pc = prev_model[2].xyz;
    vec3 r0 = cross(pb, pc), r1 = cross(pc, pa), r2 = cross(pa, pb);
    float det = dot(pa, r0);
    mat3 prev_normal_matrix = mat3(r0, r1, r2) * (abs(det) > 1e-20 ? 1.0 / det : 0.0);
#endif
    vec4 prev_local = vec4(in_pos, 1.0);
    vec3 prev_local_normal = in_normal;
#ifdef VX_SKINNED
    mat4 prev_skin = victus_skin(uint(in_normal1.w + 0.5), in_joints, in_weights);
    prev_local = prev_skin * prev_local;
    prev_local_normal = mat3(prev_skin) * prev_local_normal;
#endif
    Vertex pv = victus_run_vertex(prev_model, prev_normal_matrix, prev_local, prev_local_normal, color, vx_material.time.z);
    v_current = frame.prev_view_proj * vec4(v.world_position, 1.0);
    v_previous = frame.prev_view_proj * vec4(pv.world_position, 1.0);
#elif defined(VX_SHADOW)
    v_uv = v.uv;
#else
    vec3 local_tangent = in_tangent.xyz;
#ifdef VX_SKINNED
    local_tangent = mat3(skin) * local_tangent;
#endif
    v_world_pos = v.world_position;
    v_normal = normalize(v.normal);
    v_tangent = vec4(normalize(mat3(model) * local_tangent), in_tangent.w);
    v_uv = v.uv;
    v_color = v.color;
    v_object = vec4(in_pos * vec3(length(in_model0.xyz), length(in_model1.xyz), length(in_model2.xyz)),
                    determinant(mat3(in_model0.xyz, in_model1.xyz, in_model2.xyz)) < 0.0 ? -1.0 : 1.0);
#ifdef VX_LIGHTMAP
    v_lightmap = vec3(in_lightmap_uv + vec2(in_normal1.w, in_normal2.w), 1.0);
#else
    v_lightmap = vec3(0.0);
#endif
#endif
}
