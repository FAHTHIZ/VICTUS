#version 450
// SPDX-License-Identifier: Apache-2.0
// Skinned shadow depth (see mesh_skinned.vert for the palette layout).
layout(location = 0) in vec3 in_pos;
layout(location = 4) in vec4 in_model0;
layout(location = 5) in vec4 in_model1;
layout(location = 6) in vec4 in_model2;
layout(location = 7) in vec4 in_model3;
layout(location = 8) in vec4 in_normal0;
layout(location = 11) in uvec4 in_joints;
layout(location = 12) in vec4 in_weights;
#if defined(VICTUS_ESSL)
// GLSL ES 3.00 has no storage buffers: the RHI mirrors the palette into an RGBA32UI texture
// (1024 texels of 16 bytes per row, four texels per matrix).
layout(set = 0, binding = 15) uniform highp usampler2D vx_storage0;
vec4 joint_column(uint t) { return uintBitsToFloat(texelFetch(vx_storage0, ivec2(int(t & 1023u), int(t >> 10u)), 0)); }
mat4 joint_matrix(uint i) {
    uint t = i * 4u;
    return mat4(joint_column(t), joint_column(t + 1u), joint_column(t + 2u), joint_column(t + 3u));
}
#define JOINT(i) joint_matrix(i)
#elif defined(VICTUS_MSL)
layout(std430, set = 0, binding = 1) readonly buffer Joints { mat4 joints[]; } palette;
#define JOINT(i) palette.joints[i]
#else
layout(std430, set = 0, binding = 0) readonly buffer Joints { mat4 joints[]; } palette;
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
layout(set = 1, binding = 0) uniform Shadow { mat4 light_view_proj; } u;
void main() {
    uint base = uint(in_normal0.w + 0.5);
    mat4 skin = victus_skin(base, in_joints, in_weights);
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    gl_Position = u.light_view_proj * (model * (skin * vec4(in_pos, 1.0)));
}
