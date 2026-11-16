#version 450
// SPDX-License-Identifier: Apache-2.0
// Soft particles: the pass has no depth attachment; the scene depth is sampled instead, so
// particles behind geometry are discarded and fade out where they meet it (no hard seams).
// The same output serves alpha (src*a + dst*(1-a)) and additive (src*a + dst) blending.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_distance;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Params {
    vec4 proj;   // scene view depth = proj.x / (depth + proj.y)
    vec4 screen; // xy: 1 / render size, z: 1 / softness (metres)
} u;
layout(location = 0) out vec4 out_color;
void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    float scene = u.proj.x / (textureLod(u_depth, gl_FragCoord.xy * u.screen.xy, 0.0).r + u.proj.y);
    float gap = scene - v_distance;
    if (gap <= 0.0) discard;
    c.a *= clamp(gap * u.screen.z, 0.0, 1.0);
    if (c.a <= 0.002) discard;
    out_color = c;
}
