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
    vec4 volumetric; // volumetric fog grid (ADR-0119): xyz froxels, w far (0 off)
} u;
// SPIR-V: after the two samplers in set 2; MSL: after the one uniform buffer.
#if defined(VICTUS_ESSL)
// GLSL ES 3.00: the RHI mirrors the storage buffer into an RGBA32UI texture (1024 texels per row).
layout(set = 2, binding = 20) uniform highp usampler2D vx_storage0;
vec4 vx_texel0(uint t) { return uintBitsToFloat(texelFetch(vx_storage0, ivec2(int(t & 1023u), int(t >> 10u)), 0)); }
#define FOG_GRID(i) vx_texel0(i)
#elif defined(VICTUS_MSL)
layout(std430, set = 2, binding = 1) readonly buffer FogGrid { vec4 fog_grid[]; };
#else
layout(std430, set = 2, binding = 2) readonly buffer FogGrid { vec4 fog_grid[]; };
#endif
#if !defined(VICTUS_ESSL)
#define FOG_GRID(i) fog_grid[i]
#endif
// Volumetric fog in front of a transparent fragment (ADR-0119): the integrated froxel grid
// (in-scattered light rgb, transmittance a) fetched trilinearly at the fragment's view depth,
// the same lookup fog_apply.frag makes for opaque pixels. GRID: xyz froxels, w grid far (0 = off).
vec4 volumetric_fog(vec2 uv, float view_depth, vec4 grid) {
    if (grid.w <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    uvec3 g = uvec3(grid.xyz);
    vec2 x = clamp(uv * grid.xy - 0.5, vec2(0.0), grid.xy - 1.0);
    uvec2 a = uvec2(floor(x));
    uvec2 b = min(a + 1u, g.xy - 1u);
    vec2 w = x - vec2(a);
    float s = sqrt(clamp(view_depth / grid.w, 0.0, 1.0)) * grid.z;
    vec4 v0 = vec4(0.0, 0.0, 0.0, 1.0), v1;
    float t;
    uint z0, z1;
    if (s < 1.0) {
        z0 = 0u;
        z1 = 0u;
        t = s;
    } else {
        z0 = min(uint(floor(s - 1.0)), g.z - 1u);
        z1 = min(z0 + 1u, g.z - 1u);
        t = s - 1.0 - float(z0);
    }
    vec4 c[2];
    for (int k = 0; k < 2; ++k) {
        uint base = (k == 0 ? z0 : z1) * g.y * g.x;
        vec4 r0 = mix(FOG_GRID(base + a.y * g.x + a.x), FOG_GRID(base + a.y * g.x + b.x), w.x);
        vec4 r1 = mix(FOG_GRID(base + b.y * g.x + a.x), FOG_GRID(base + b.y * g.x + b.x), w.x);
        c[k] = mix(r0, r1, w.y);
    }
    v1 = s < 1.0 ? mix(v0, c[0], t) : mix(c[0], c[1], t);
    v1.a = clamp(v1.a, 0.0, 1.0);
    return v1;
}

layout(location = 0) out vec4 out_color;
// VICTUS_REACTIVE_VARIANT: the "#reactive" build also writes the TAA reactive mask (ADR-0143), an
// R8 second target blended like the colour: how much the resolve should trust this frame here.
#ifdef VICTUS_REACTIVE
layout(location = 1) out vec4 out_reactive;
#endif

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
    // Volumetric fog in front of the particle (ADR-0119), drawn after the fog composite.
    vec4 vf = volumetric_fog(gl_FragCoord.xy * u.screen.xy, v_distance, u.volumetric);
    c.rgb = u.screen.w > 0.5 ? c.rgb * vf.a : c.rgb * vf.a + vf.rgb;
    out_color = c;
#ifdef VICTUS_REACTIVE
    // Alpha: coverage accumulated with the same "over" blend. Additive: light added on top, blended
    // additively, so faint glows stay mostly history-resolved and bright ones trust this frame.
    if (u.screen.w > 0.5) out_reactive = vec4(vec3(c.a * min(dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) * 4.0, 1.0)), 1.0);
    else out_reactive = vec4(1.0, 1.0, 1.0, c.a);
#endif
}
