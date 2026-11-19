#version 450
// SPDX-License-Identifier: Apache-2.0
// Rain streaks and snow flakes (ADR-0113): soft profile, tested against the sampled scene depth
// (the pass has no depth attachment) and faded where drops meet geometry, hidden where the rain
// occlusion map has something between the drop and the sky (ADR-0114), alpha blended. Also draws
// rain splashes from splash.vert (ADR-0118).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_distance;
layout(location = 3) in vec3 v_world;
layout(set = 2, binding = 0) uniform sampler2D u_depth;
layout(set = 2, binding = 1) uniform sampler2DShadow u_rain_occlusion; // ADR-0114
layout(set = 3, binding = 0) uniform Params {
    vec4 proj;   // scene view depth = proj.x / (depth + proj.y); z rain occlusion on, w its depth bias
    vec4 screen; // xy: 1 / render size, z: 1 / softness (metres), w type (0 rain, 1 snow, 2 splash)
    mat4 rain_vp; // world -> rain occlusion map
    vec4 fog_color;  // distance fog (ADR-0116): rgb radiance, w mode
    vec4 fog_params; // x start, y end, z density, w max opacity
    vec4 view;       // xy: 1 / projection x and y scale
} u;
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

layout(location = 0) out vec4 out_color;
void main() {
    float shape;
    if (u.screen.w > 1.5) { // splash crown (ADR-0118): a ring around the landing point at the bottom edge
        float r = length(vec2(v_uv.x, v_uv.y + 1.0) * vec2(1.0, 0.5));
        shape = smoothstep(0.45, 0.8, r) * (1.0 - smoothstep(0.85, 1.0, r));
    } else if (u.screen.w > 0.5) shape = 1.0 - smoothstep(0.4, 1.0, length(v_uv));
    else shape = (1.0 - abs(v_uv.x)) * (1.0 - v_uv.y * v_uv.y);
    float a = v_color.a * shape;
    if (a <= 0.002) discard;
    if (u.proj.z > 0.5) { // under a roof (or anything else between the drop and the sky)
        vec4 r4 = u.rain_vp * vec4(v_world, 1.0);
        vec3 r = r4.xyz / r4.w;
        vec2 ruv = r.xy * vec2(0.5, -0.5) + 0.5;
        if (all(greaterThanEqual(ruv, vec2(0.0))) && all(lessThanEqual(ruv, vec2(1.0))) && r.z <= 1.0) {
            a *= texture(u_rain_occlusion, vec3(ruv, r.z - u.proj.w));
            if (a <= 0.002) discard;
        }
    }
    float scene = u.proj.x / (textureLod(u_depth, gl_FragCoord.xy * u.screen.xy, 0.0).r + u.proj.y);
    float gap = scene - v_distance;
    if (gap <= 0.0) discard;
    a *= clamp(gap * u.screen.z, 0.0, 1.0);
    out_color = vec4(mix(v_color.rgb, u.fog_color.rgb, distance_fog(v_distance)), a);
}
