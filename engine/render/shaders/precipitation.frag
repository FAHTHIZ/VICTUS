#version 450
// SPDX-License-Identifier: Apache-2.0
// Rain streaks and snow flakes (ADR-0113): soft profile, tested against the sampled scene depth
// (the pass has no depth attachment) and faded where drops meet geometry, alpha blended.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_distance;
layout(set = 2, binding = 0) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Params {
    vec4 proj;   // scene view depth = proj.x / (depth + proj.y)
    vec4 screen; // xy: 1 / render size, z: 1 / softness (metres), w type (0 rain, 1 snow)
} u;
layout(location = 0) out vec4 out_color;
void main() {
    float shape;
    if (u.screen.w > 0.5) shape = 1.0 - smoothstep(0.4, 1.0, length(v_uv));
    else shape = (1.0 - abs(v_uv.x)) * (1.0 - v_uv.y * v_uv.y);
    float a = v_color.a * shape;
    if (a <= 0.002) discard;
    float scene = u.proj.x / (textureLod(u_depth, gl_FragCoord.xy * u.screen.xy, 0.0).r + u.proj.y);
    float gap = scene - v_distance;
    if (gap <= 0.0) discard;
    a *= clamp(gap * u.screen.z, 0.0, 1.0);
    out_color = vec4(v_color.rgb, a);
}
