#version 450
// SPDX-License-Identifier: Apache-2.0
// Soft particles: the pass has no depth attachment; the scene depth is sampled instead, so
// particles behind geometry are discarded and fade out where they meet it (no hard seams).
// The same output serves alpha (src*a + dst*(1-a)) and additive (src*a + dst) blending.
// Distance fog is applied per fragment like lit surfaces (ADR-0116).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_distance;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Params {
    vec4 proj;   // scene view depth = proj.x / (depth + proj.y)
    vec4 screen; // xy: 1 / render size, z: 1 / softness (metres), w: 1 for additive blending
    vec4 fog_color;  // rgb fog radiance, w mode (0 off, 1 linear, 2 exp, 3 exp2)
    vec4 fog_params; // x start, y end, z density, w max opacity
    vec4 view;       // xy: 1 / projection x and y scale (view ray per unit depth)
} u;
layout(location = 0) out vec4 out_color;

// Distance fog (ADR-0116), matching pbr.frag: range along the pixel's ray from the view depth.
float distance_fog(float view_depth) {
    int mode = int(u.fog_color.w + 0.5);
    if (mode == 0) return 0.0;
    vec2 ndc = gl_FragCoord.xy * u.screen.xy * 2.0 - 1.0;
    float d = view_depth * length(vec3(ndc * u.view.xy, 1.0));
    float f;
    if (mode == 1) f = clamp((d - u.fog_params.x) / max(u.fog_params.y - u.fog_params.x, 1e-3), 0.0, 1.0);
    else {
        float k = max(d - u.fog_params.x, 0.0) * u.fog_params.z;
        f = mode == 2 ? 1.0 - exp(-k) : 1.0 - exp(-k * k);
    }
    return f * u.fog_params.w;
}
void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    float scene = u.proj.x / (textureLod(u_depth, gl_FragCoord.xy * u.screen.xy, 0.0).r + u.proj.y);
    float gap = scene - v_distance;
    if (gap <= 0.0) discard;
    c.a *= clamp(gap * u.screen.z, 0.0, 1.0);
    if (c.a <= 0.002) discard;
    // Fog between the camera and the particle: alpha particles take on the fog colour, additive
    // ones (light added on top of an already fogged scene) are dimmed by the same amount.
    float f = distance_fog(v_distance);
    c.rgb = u.screen.w > 0.5 ? c.rgb * (1.0 - f) : mix(c.rgb, u.fog_color.rgb, f);
    out_color = c;
}
