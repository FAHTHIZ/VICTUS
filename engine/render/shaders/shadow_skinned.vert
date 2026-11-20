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
layout(set = 1, binding = 0) uniform Shadow { mat4 light_view_proj; } u;
void main() {
    uint base = uint(in_normal0.w + 0.5);
    mat4 skin = in_weights.x * JOINT(base + in_joints.x) + in_weights.y * JOINT(base + in_joints.y) +
                in_weights.z * JOINT(base + in_joints.z) + in_weights.w * JOINT(base + in_joints.w);
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    gl_Position = u.light_view_proj * (model * (skin * vec4(in_pos, 1.0)));
}
