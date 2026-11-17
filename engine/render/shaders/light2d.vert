#version 450
// SPDX-License-Identifier: Apache-2.0
// 2D light quad (ADR-0065): a square of half-size `radius` around the light, from gl_VertexIndex.
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 center_radius; // xy world centre, z radius
} frame;
layout(location = 0) out vec2 v_offset; // world offset from the light centre
void main() {
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 o = corners[gl_VertexIndex % 6] * frame.center_radius.z;
    v_offset = o;
    gl_Position = frame.view_proj * vec4(frame.center_radius.xy + o, 0.0, 1.0);
}
