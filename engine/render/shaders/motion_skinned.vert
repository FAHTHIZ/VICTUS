#version 450
// SPDX-License-Identifier: Apache-2.0
// Skinned object motion vectors: motion.vert plus the current and previous joint palettes (both in
// one storage buffer; in_params.x / .y are their first matrices). The current position uses the
// same arithmetic as mesh_skinned.vert / depth_skinned.vert (invariant).
layout(location = 0) in vec3 in_pos;
layout(location = 4) in vec4 in_model0;
layout(location = 5) in vec4 in_model1;
layout(location = 6) in vec4 in_model2;
layout(location = 7) in vec4 in_model3;
layout(location = 8) in vec4 in_prev_row0;
layout(location = 9) in vec4 in_prev_row1;
layout(location = 10) in vec4 in_prev_row2;
layout(location = 11) in uvec4 in_joints;
layout(location = 12) in vec4 in_weights;
layout(location = 13) in vec4 in_params; // x: current palette base, y: previous palette base
#ifdef VICTUS_MSL
layout(std430, set = 0, binding = 1) readonly buffer Joints { mat4 joints[]; } palette;
#else
layout(std430, set = 0, binding = 0) readonly buffer Joints { mat4 joints[]; } palette;
#endif
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    mat4 prev_view_proj;
} frame;
layout(location = 0) out vec4 v_current;
layout(location = 1) out vec4 v_previous;
invariant gl_Position;
void main() {
    uint base = uint(in_params.x + 0.5);
    mat4 skin = in_weights.x * palette.joints[base + in_joints.x] + in_weights.y * palette.joints[base + in_joints.y] +
                in_weights.z * palette.joints[base + in_joints.z] + in_weights.w * palette.joints[base + in_joints.w];
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    vec4 world = model * (skin * vec4(in_pos, 1.0));
    gl_Position = frame.view_proj * world;
    uint pb = uint(in_params.y + 0.5);
    mat4 prev_skin = in_weights.x * palette.joints[pb + in_joints.x] + in_weights.y * palette.joints[pb + in_joints.y] +
                     in_weights.z * palette.joints[pb + in_joints.z] + in_weights.w * palette.joints[pb + in_joints.w];
    vec4 p = prev_skin * vec4(in_pos, 1.0);
    vec3 prev_world = vec3(dot(in_prev_row0, p), dot(in_prev_row1, p), dot(in_prev_row2, p));
    v_current = frame.prev_view_proj * world;
    v_previous = frame.prev_view_proj * vec4(prev_world, 1.0);
}
