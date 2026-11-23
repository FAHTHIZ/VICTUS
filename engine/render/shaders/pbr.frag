#version 450
// SPDX-License-Identifier: Apache-2.0
// Metallic-roughness PBR (glTF 2.0 model): GGX/Smith/Schlick, directional sun with cascaded
// shadow maps (2x2 atlas, PCF, dithered cascade transitions), clustered forward point and spot lights,
// image-based lighting from a baked environment map (GGX-prefiltered cubemap + SH9 irradiance)
// or the analytic sky (diffuse + split-sum specular approximation), screen-space ambient
// occlusion and emissive.
//
// Project surface shaders (.vshader, ADR-0035) are spliced in at VICTUS_SURFACE_CODE: they
// define `void surface(SurfaceInput i, inout Surface s)`, which runs after the material's
// textures were sampled and before lighting. The interface below is a stable contract.
// per-vertex colour, a weather coverage layer (snow, dust, ash) on
// upward faces and distance fog.
//
// VICTUS_FP16_VARIANT: compile_shaders.py also builds this shader with VICTUS_FP16 defined
// (pbr.frag#fp16, ADR-0137). That variant evaluates the BRDF terms in explicit half precision
// (float16_t) for GPUs with shaderFloat16; positions, directions, dot products, shadow and fog
// maths and light radiance stay FP32. `mfloat`/`mvec3` are float/vec3 in the default build.
#ifdef VICTUS_FP16
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#define mfloat float16_t
#define mvec2 f16vec2
#define mvec3 f16vec3
#define mvec4 f16vec4
#else
#define mfloat float
#define mvec2 vec2
#define mvec3 vec3
#define mvec4 vec4
#endif
layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tangent;
layout(location = 4) in vec4 v_color; // linear rgb, alpha: opacity or coverage mask (material.flags.z)
// Baked lightmap (ADR-0122): xy atlas uv, z 1 when the mesh is lightmapped (mesh_lightmap.vert), else 0.
layout(location = 5) in vec3 v_lightmap;

layout(set = 2, binding = 0) uniform sampler2D u_base_color;
layout(set = 2, binding = 1) uniform sampler2D u_metal_rough;
layout(set = 2, binding = 2) uniform sampler2D u_normal_map;
layout(set = 2, binding = 3) uniform sampler2D u_emissive;
layout(set = 2, binding = 4) uniform sampler2DShadow u_shadow;
layout(set = 2, binding = 5) uniform sampler2D u_ao;
layout(set = 2, binding = 6) uniform samplerCube u_env; // prefiltered environment (black 1x1 when absent)
layout(set = 2, binding = 7) uniform sampler2DShadow u_local_shadow; // spot/point shadow atlas (ADR-0052)
layout(set = 2, binding = 8) uniform sampler2D u_irradiance; // irradiance volume probes (ADR-0056)
layout(set = 2, binding = 9) uniform sampler2DShadow u_rain_occlusion; // depth along the rain (ADR-0114)
layout(set = 2, binding = 10) uniform sampler2D u_lightmap; // baked irradiance / pi atlas (ADR-0122), black 1x1 when absent
// Directional lightmap (ADR-0130): rgb dominant direction * 0.5 + 0.5, a rebalancing term (0 = none).
layout(set = 2, binding = 11) uniform sampler2D u_lightmap_dir;

// Clustered local lights (see light_clusters.hpp): a light array and, per cluster, an
// (offset, count) header followed by the flattened light index list.
// spot: x cos(inner), y 1 = spot, z first shadow tile + 1 (0 = unshadowed), w shadow texel angle.
// Shadow matrices (world -> atlas uv/depth) follow the lights in the same array, one per tile.
#ifndef VX_CUSTOM_TEXTURES
#define VX_CUSTOM_TEXTURES 0
#endif
struct LocalLight { vec4 position_range; vec4 color_intensity; vec4 direction_cos_outer; vec4 spot; };
// SDL_GPU: SPIR-V fragment storage buffers follow the 11 textures in set 2; MSL buffers are the
// 3 uniform buffers first, then storage buffers ([[buffer(3)]], [[buffer(4)]]).
#if defined(VICTUS_ESSL)
// GLSL ES 3.00 has no storage buffers: the RHI mirrors each into an RGBA32UI texture
// (1024 texels of 16 bytes per row; a light is four texels, a cluster word one component).
layout(set = 2, binding = 20) uniform highp usampler2D vx_storage0;
vec4 vx_texel0(uint t) { return uintBitsToFloat(texelFetch(vx_storage0, ivec2(int(t & 1023u), int(t >> 10u)), 0)); }
layout(set = 2, binding = 21) uniform highp usampler2D vx_storage1;
vec4 vx_texel1(uint t) { return uintBitsToFloat(texelFetch(vx_storage1, ivec2(int(t & 1023u), int(t >> 10u)), 0)); }
layout(set = 2, binding = 22) uniform highp usampler2D vx_storage2;
vec4 vx_texel2(uint t) { return uintBitsToFloat(texelFetch(vx_storage2, ivec2(int(t & 1023u), int(t >> 10u)), 0)); }
LocalLight local_light(uint i) {
    uint t = i * 4u;
    return LocalLight(vx_texel0(t), vx_texel0(t + 1u), vx_texel0(t + 2u), vx_texel0(t + 3u));
}
uint cluster_word(uint i) { return texelFetch(vx_storage1, ivec2(int((i >> 2u) & 1023u), int(i >> 12u)), 0)[i & 3u]; }
layout(set = 2, binding = 23) uniform highp usampler2D vx_storage3;
uint cookie_word(uint i) { return texelFetch(vx_storage3, ivec2(int((i >> 2u) & 1023u), int(i >> 12u)), 0)[i & 3u]; }
#define LOCAL_LIGHT(i) local_light(i)
#define CLUSTER(i) cluster_word(i)
#define FOG_GRID(i) vx_texel2(i)
#define COOKIE(i) cookie_word(i)
#elif defined(VICTUS_MSL)
layout(std430, set = 2, binding = 3) readonly buffer Lights { LocalLight local_lights[]; };
layout(std430, set = 2, binding = 4) readonly buffer Clusters { uint cluster_data[]; };
layout(std430, set = 2, binding = 5) readonly buffer FogGrid { vec4 fog_grid[]; }; // ADR-0119
layout(std430, set = 2, binding = 6) readonly buffer Cookies { uint cookie_data[]; }; // ADR-0195
#else
// Surface shader textures (ADR-0171) take bindings 12.., the two screen-space reflection inputs
// follow them (ADR-0199), then the storage buffers.
layout(std430, set = 2, binding = 14 + VX_CUSTOM_TEXTURES) readonly buffer Lights { LocalLight local_lights[]; };
layout(std430, set = 2, binding = 15 + VX_CUSTOM_TEXTURES) readonly buffer Clusters { uint cluster_data[]; };
layout(std430, set = 2, binding = 16 + VX_CUSTOM_TEXTURES) readonly buffer FogGrid { vec4 fog_grid[]; }; // ADR-0119
layout(std430, set = 2, binding = 17 + VX_CUSTOM_TEXTURES) readonly buffer Cookies { uint cookie_data[]; }; // ADR-0195
#endif
#if !defined(VICTUS_ESSL)
// ADR-0199: last frame's TAA-resolved HDR colour and linear depth history, for screen-space
// reflections on opaque glossy surfaces. GLSL ES has no spare texture units for them.
#define VX_SSR 1
layout(set = 2, binding = 12 + VX_CUSTOM_TEXTURES) uniform sampler2D u_ssr_color;
layout(set = 2, binding = 13 + VX_CUSTOM_TEXTURES) uniform sampler2D u_ssr_depth;
#endif
#if !defined(VICTUS_ESSL)
#define LOCAL_LIGHT(i) local_lights[i]
#define CLUSTER(i) cluster_data[i]
#define FOG_GRID(i) fog_grid[i]
#define COOKIE(i) cookie_data[i]
#endif
layout(set = 3, binding = 0) uniform Lighting {
    vec4 camera_pos;     // w: screen-space occlusion texture: 0 none, 1 ambient occlusion (r), 2 also sun contact shadows (g, ADR-0183)
    vec4 camera_forward; // xyz: view direction, w: material texture mip bias (ADR-0141, < 0 when upscaling)
    vec4 sun_dir;        // xyz: direction light travels, w: shadow enabled
    vec4 sun_color;      // rgb * intensity
    vec4 sky_color;      // rgb zenith ambient radiance, w: exposure
    vec4 horizon_color;  // rgb horizon ambient radiance
    vec4 ground_color;   // rgb ground ambient radiance, w: local light count
    vec4 shadow_params;  // x: atlas texel size, y: depth bias, z: normal bias (texels), w: cascade count
    vec4 cascade_params; // x: soft (3x3 PCF), y: atlas tile scale, z: transition fraction, w: lightmap alpha is a sun shadowmask (ADR-0131)
    vec4 cascade_splits; // far view depth of each cascade
    vec4 cascade_texel;  // world-space texel size of each cascade
    vec4 screen;         // xy: 1 / render size, z: time in seconds (wraps every hour), w: compact HDR frame + 1 (0 off)
    vec4 fog_color;      // rgb fog radiance, w: mode (0 off, 1 linear, 2 exponential, 3 exponential squared)
    vec4 fog_params;     // x start distance, y end distance (linear), z density (exp), w max opacity
    vec4 coverage_color; // rgb coverage albedo (linear), w: global coverage amount 0..1
    vec4 coverage_params; // x coverage roughness, y slope threshold (normal.y), z slope sharpness
    vec4 env_params;     // x: environment map present, y: max mip, z: intensity, w: rotation (radians about +Y)
    vec4 env_sh[9];      // SH9 irradiance / pi (rgb), pre-multiplied by the intensity
    mat4 cascade_vp[4];
    vec4 cluster_grid;   // xyz: tiles x, tiles y, depth slices, w: max lights per cluster
    vec4 cluster_depth;  // x: near, y: slices / log(far / near)
    vec4 local_shadow;   // x: atlas texel size, y: tiles per row, z: index of the first shadow matrix, w: enabled
    vec4 gi_min;         // irradiance volume: xyz box min, w: active
    vec4 gi_max;         // xyz box max, w: intensity
    vec4 gi_counts;      // xyz probes per axis, w: normal offset (metres)
    vec4 cloud_layer;    // volumetric clouds (ADR-0112): x bottom, y top, z coverage, w extinction per metre
    vec4 cloud_shadow;   // x 1 / shape size, y shadow strength, z enabled
    vec4 cloud_wind;     // xyz shape offset (metres)
    vec4 weather;        // x surface wetness 0..1 (ADR-0113), y rain occlusion on, z its depth bias (ADR-0114), w puddle amount (ADR-0117)
    mat4 rain_vp;        // world -> rain occlusion map
    vec4 puddles;        // x 1 / puddle size (m), y ripple strength, z 1 / ripple cell size (m), w ripples per second
    vec4 volumetric;     // volumetric fog grid for blended draws (ADR-0119): xyz froxels, w far (0 off)
    mat4 ssr_prev_vp;    // ADR-0199: world -> previous frame clip (unjittered); clip w is the history's linear depth
    vec4 ssr_params;     // x ray steps (0 off), y max distance (m), z thickness (m), w max roughness
    vec4 ssr_frame;      // x frame index (noise offset), y history intensity scale
    vec4 sun_cookie;     // ADR-0201: x atlas slot + 1 (0 none), y 1 / tile size (m), zw offset (m)
    vec4 sun_cookie_right; // xyz right axis across the sun (up = cross(right, sun_dir))
} lighting;

// Stochastic rounding to R11G11B10_FLOAT (compact HDR, ADR-0129). The result is exactly
// representable, so the hardware conversion (round-to-nearest or toward zero, which Vulkan leaves
// to the implementation) cannot bias it, and repeated stores (TAA history) do not drift darker.
// Interleaved gradient noise (Jimenez 2014) per channel, offset per frame.
vec3 hdr_quantize(vec3 c, float frame) {
    vec2 p = gl_FragCoord.xy + 5.588238 * frame;
    const vec2 k = vec2(0.06711056, 0.00583715);
    vec3 n = vec3(fract(52.9829189 * fract(dot(p, k))), fract(52.9829189 * fract(dot(p + vec2(17.0, 59.0), k))),
                  fract(52.9829189 * fract(dot(p + vec2(83.0, 31.0), k))));
    c = clamp(c, vec3(0.0), vec3(64512.0));
#ifdef VICTUS_ESSL
    // GLSL ES 3.00 has no frexp/ldexp: the exponent comes from the float bits, and the scales are
    // exact powers of two built from bits, so the products are exact as with ldexp.
    ivec3 e = ((floatBitsToInt(max(c, vec3(1e-30))) >> 23) & 0xff) - 126;
    ivec3 sh = max(e - 1, ivec3(-14)) - ivec3(6, 6, 5);
    return floor(c * intBitsToFloat((127 - sh) << 23) + n) * intBitsToFloat((sh + 127) << 23);
#else
    ivec3 e;
    frexp(max(c, vec3(1e-30)), e); // c = m * 2^e with m in [0.5, 1)
    // Scale by exact powers of two with ldexp, not exp2 and a division: Metal compiles with fast
    // math by default, where exp2 and division are approximate, so the result was not exactly
    // representable and truncating stores biased it dark again.
    ivec3 sh = max(e - 1, ivec3(-14)) - ivec3(6, 6, 5);
    return ldexp(floor(ldexp(c, -sh) + n), sh);
#endif
}
layout(set = 3, binding = 1) uniform Material {
    vec4 base_color;
    vec4 emissive;       // rgb * strength
    vec4 params;         // x metallic, y roughness, z normal scale, w alpha cutoff
    vec4 flags;          // x has normal map, y unlit, z vertex alpha is a coverage mask, w coverage response
    vec4 uv_transform;   // xy scale, zw offset (material tiling)
    vec4 custom[4];      // surface shader parameters (material "shader_params")
    vec4 blend;          // x: 0 opaque, 1 alpha blended, 2 additive (volumetric fog, ADR-0119); y: ORM occlusion strength (ADR-0139),
                         // z: alpha to coverage, w: LOD cross-fade (ADR-0184; > 0 keeps dither < w, < 0 keeps dither >= -w)
    vec4 mapping;        // x: 1 = world-space triplanar mapping (ADR-0197), y: blend sharpness
    vec4 layers;         // x: clear coat strength (ADR-0203), y: clear coat roughness
    vec4 sheen;          // rgb: sheen colour, linear (ADR-0231, KHR_materials_sheen; 0 = none), a: sheen roughness
    vec4 anisotropy;     // x: anisotropy strength 0..1 (ADR-0232, KHR_materials_anisotropy; 0 = none), y: rotation (radians, from the tangent)
    vec4 subsurface;     // rgb: subsurface scattering colour, linear (ADR-0233; 0 = none), a: thickness 0..1
} material;

// Local reflection probe for this draw (box-projected cube bound to u_env instead of the scene's
// environment), or inactive (box_min.w == 0).
layout(set = 3, binding = 2) uniform Probe {
    vec4 box_min;  // xyz world box, w: active
    vec4 box_max;  // xyz, w: max mip
    vec4 position; // xyz capture point, w: intensity
    vec4 sh[9];    // SH9 irradiance / pi captured at the probe, pre-multiplied by the intensity
} probe;

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

// Blended draws are composited after the volumetric fog, so they take their own share of it:
// alpha surfaces become colour * transmittance + in-scattered light, additive ones are dimmed.
// Opaque draws are left alone (fog_apply.frag covers them by the depth buffer).
vec3 blended_fog(vec3 color) {
    if (material.blend.x < 0.5 || lighting.volumetric.w <= 0.0) return color;
    float view_depth = dot(v_world_pos - lighting.camera_pos.xyz, lighting.camera_forward.xyz);
    vec4 f = volumetric_fog(gl_FragCoord.xy * lighting.screen.xy, view_depth, lighting.volumetric);
    return material.blend.x > 1.5 ? color * f.a : color * f.a + f.rgb;
}
layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;

// What the lighting uses. Surface shaders may change any field.
struct Surface {
    vec3 base_color; // linear albedo
    float alpha;     // < the material's alpha cutoff discards the pixel
    vec3 normal;     // world space, unit length, facing the viewer
    float metallic;
    float roughness;
    vec3 emissive;   // linear radiance added after lighting
    float occlusion; // multiplies ambient light (1 = unoccluded)
    bool unlit;      // output base_color + emissive without lighting
};
// Read-only inputs for surface shaders.
struct SurfaceInput {
    vec2 uv;             // material-transformed texture coordinates
    vec2 mesh_uv;        // the mesh's own texture coordinates
    vec3 world_position;
    vec3 view_dir;       // unit vector towards the camera
    vec3 vertex_normal;  // interpolated geometric normal (world space)
    float time;          // seconds
    vec4 params[4];      // the material's shader parameters
    vec4 vertex_color;   // interpolated vertex colour (rgb linear; alpha as stored)
};

// VICTUS_SURFACE_CODE

float distribution_ggx(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-7);
}
#ifdef VICTUS_FP16
// Half-precision GGX (Filament's mobile form): 1 - NdotH^2 = |N x H|^2 is computed in FP32,
// where it does not cancel out, and the perceptual roughness is clamped to 0.089 so that the
// specular peak (1 / (pi a^2)) stays far below the half-float maximum.
const float kMinRoughnessFp16 = 0.089;
mfloat distribution_ggx_h(float one_minus_nh2, float NdotH, mfloat a) {
    mfloat an = mfloat(NdotH) * a;
    mfloat k = a / (mfloat(one_minus_nh2) + an * an);
    return min(k * k * mfloat(1.0 / PI), mfloat(60000.0));
}
#endif
float geometry_smith(float NdotV, float NdotL, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}
vec3 fresnel_schlick(float cos_theta, vec3 f0) { return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0); }

// Analytic environment: the same zenith/horizon/ground gradient as the sky, blurred towards
// the hemispherical average as roughness increases.
vec3 environment(vec3 dir, float roughness) {
    float h = clamp(dir.y, -1.0, 1.0);
    vec3 sharp = h > 0.0 ? mix(lighting.horizon_color.rgb, lighting.sky_color.rgb, sqrt(h))
                         : mix(lighting.horizon_color.rgb, lighting.ground_color.rgb, pow(-h, 0.4));
    vec3 blurred = mix(lighting.ground_color.rgb, lighting.sky_color.rgb, h * 0.5 + 0.5) * 0.5 + lighting.horizon_color.rgb * 0.5;
    return mix(sharp, blurred, roughness * roughness);
}
// Environment directions are rotated about +Y by env_params.w (the scene's environment yaw).
vec3 env_rotate(vec3 d) {
    float c = cos(lighting.env_params.w), s = sin(lighting.env_params.w);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}
vec3 sh9(vec4 c[9], vec3 n) {
    vec3 r = c[0].rgb * 0.282095
           + c[1].rgb * (0.488603 * n.y) + c[2].rgb * (0.488603 * n.z)
           + c[3].rgb * (0.488603 * n.x) + c[4].rgb * (1.092548 * n.x * n.y)
           + c[5].rgb * (1.092548 * n.y * n.z) + c[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0))
           + c[7].rgb * (1.092548 * n.x * n.z) + c[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(r, vec3(0.0));
}

// Irradiance volume (ADR-0056): SH9 probes on a grid, trilinear between them. Tile layout: Y
// layers side by side, coefficients stacked; rgb = coefficient * validity, a = validity
// (row 1: sun visibility * validity, the probe sun occlusion of ADR-0136).
vec4 gi_fetch(int k, float layer, vec2 fxz) {
    vec2 size = vec2(lighting.gi_counts.x * lighting.gi_counts.y, lighting.gi_counts.z * 9.0);
    vec2 t = vec2(layer * lighting.gi_counts.x + fxz.x + 0.5, float(k) * lighting.gi_counts.z + fxz.y + 0.5);
    return textureLod(u_irradiance, t / size, 0.0);
}
// `sun_vis`: the probes' baked sun visibility, 1 outside the volume (blended like the irradiance).
vec3 volume_irradiance(vec3 p, vec3 n, vec3 fallback, out float sun_vis) {
    sun_vis = 1.0;
    if (lighting.gi_min.w < 0.5) return fallback;
    vec3 counts = lighting.gi_counts.xyz;
    vec3 lo = lighting.gi_min.xyz, hi = lighting.gi_max.xyz;
    p += n * lighting.gi_counts.w; // offset off the surface: fewer probes from behind walls
    vec3 eps = (hi - lo) * 1e-4;
    if (any(lessThan(p, lo - eps)) || any(greaterThan(p, hi + eps))) return fallback;
    vec3 f = clamp((p - lo) / (hi - lo) * (counts - 1.0), vec3(0.0), counts - 1.0);
    float y0 = min(floor(f.y), counts.y - 2.0);
    float ty = f.y - y0;
    vec4 c[9];
    // gi_min.w = 2: L1 only (Low, ADR-0138), the four band 0-1 rows, 8 fetches instead of 18.
    int bands = lighting.gi_min.w > 1.5 ? 4 : 9;
    for (int k = 0; k < 9; ++k) c[k] = k < bands ? mix(gi_fetch(k, y0, f.xz), gi_fetch(k, y0 + 1.0, f.xz), ty) : vec4(0.0);
    float valid = c[0].a;
    if (valid < 1e-3) return fallback;
    for (int k = 0; k < 9; ++k) c[k].rgb /= valid;
    vec3 v = sh9(c, n) * lighting.gi_max.w;
    sun_vis = mix(1.0, clamp(c[1].a / valid, 0.0, 1.0), min(valid, 1.0));
    return mix(fallback, v, min(valid, 1.0));
}
vec3 env_irradiance(vec3 n) {
    if (probe.box_min.w > 0.5) return sh9(probe.sh, n);
    if (lighting.env_params.x < 0.5) return environment(n, 1.0);
    return sh9(lighting.env_sh, env_rotate(n));
}
// Parallax-corrected (box-projected) reflection direction (Lagarde & Zanuttini 2012).
vec3 probe_direction(vec3 p, vec3 r) {
    vec3 inv = 1.0 / max(abs(r), vec3(1e-5)) * sign(r + vec3(1e-7));
    vec3 t = max((probe.box_max.xyz - p) * inv, (probe.box_min.xyz - p) * inv);
    float d = max(min(min(t.x, t.y), t.z), 0.0);
    return p + r * d - probe.position.xyz;
}
vec3 env_specular(vec3 r, float roughness, vec3 p) {
    if (probe.box_min.w > 0.5) return textureLod(u_env, probe_direction(p, r), roughness * probe.box_max.w).rgb * probe.position.w;
    if (lighting.env_params.x < 0.5) return environment(r, roughness);
    return textureLod(u_env, env_rotate(r), roughness * lighting.env_params.y).rgb * lighting.env_params.z;
}
#ifdef VX_SSR
// Screen-space reflection against the previous frame (ADR-0199). The reflected ray is marched in
// world space with quadratically growing steps and each point is projected with last frame's
// view-projection, where clip w is the linear depth the TAA depth history stores. A point is a hit
// when it lies behind the stored surface by less than the thickness plus 2% of its depth plus the
// step length; five bisection steps refine it. Returns the history colour and, in w, a confidence
// that fades at the screen edges, near the maximum distance and towards the roughness limit.
vec4 ssr_trace(vec3 p, vec3 r, vec3 n, float roughness) {
    vec4 sp = lighting.ssr_params;
    if (sp.x < 0.5 || roughness >= sp.w) return vec4(0.0);
    int steps = int(sp.x + 0.5);
    vec2 q = gl_FragCoord.xy + 5.588238 * lighting.ssr_frame.x;
    float jitter = fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715))));
    vec3 origin = p + n * 0.02;
    float t_prev = 0.0, t_hit = -1.0;
    for (int i = 0; i < steps; ++i) {
        float s = (float(i) + jitter) / float(steps);
        float t = max(s * s * sp.y, 0.05);
        vec4 c = lighting.ssr_prev_vp * vec4(origin + r * t, 1.0);
        if (c.w <= 0.0) break;
        vec2 uv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float scene = textureLod(u_ssr_depth, uv, 0.0).r;
        float behind = c.w - scene;
        if (behind > 0.0 && behind < sp.z + 0.02 * c.w + (t - t_prev)) {
            float lo = t_prev, hi = t;
            for (int k = 0; k < 5; ++k) {
                float mid = 0.5 * (lo + hi);
                vec4 m = lighting.ssr_prev_vp * vec4(origin + r * mid, 1.0);
                vec2 muv = vec2(m.x / m.w * 0.5 + 0.5, 0.5 - m.y / m.w * 0.5);
                if (m.w > textureLod(u_ssr_depth, muv, 0.0).r) hi = mid;
                else lo = mid;
            }
            t_hit = hi;
            break;
        }
        // A step that jumps far behind a surface passed under an object: keep marching.
        t_prev = t;
    }
    if (t_hit < 0.0) return vec4(0.0);
    vec4 c = lighting.ssr_prev_vp * vec4(origin + r * t_hit, 1.0);
    vec2 uv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    vec2 edge = min(uv, 1.0 - uv);
    float conf = clamp(min(edge.x, edge.y) / 0.08, 0.0, 1.0);
    conf *= clamp((1.0 - t_hit / sp.y) / 0.25, 0.0, 1.0);
    conf *= 1.0 - smoothstep(sp.w * 0.6, sp.w, roughness);
    vec3 hit = min(textureLod(u_ssr_color, uv, 0.0).rgb * lighting.ssr_frame.y, vec3(64.0));
    return vec4(hit, conf);
}
#endif
// Karis' analytic approximation of the split-sum environment BRDF (mobile). FP32 in every variant:
// its scale/bias multiply the whole environment specular, and in half precision the bias term
// (1.04 * a004 + r.w) loses most of its bits to cancellation (ADR-0153). It is five ALU ops.
vec2 env_brdf(float NdotV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 brdf(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
#ifdef VICTUS_FP16
    // Half-precision terms (ADR-0137); the result is multiplied by the FP32 radiance, so bright
    // lights cannot overflow.
    vec3 NxH = cross(N, H);
    mfloat r = mfloat(max(roughness, kMinRoughnessFp16));
    mfloat m = mfloat(metallic);
    mvec3 a = mvec3(albedo);
    mvec3 f0h = mix(mvec3(0.04), a, m);
    mfloat hv = mfloat(max(dot(H, V), 0.0));
    mfloat fw = mfloat(1.0) - hv;
    mfloat fw2 = fw * fw;
    mvec3 Fh = f0h + (mvec3(1.0) - f0h) * (fw2 * fw2 * fw);
    mfloat Dh = distribution_ggx_h(dot(NxH, NxH), NdotH, r * r);
    mfloat kh = (r + mfloat(1.0)) * (r + mfloat(1.0)) * mfloat(0.125);
    mfloat nv = mfloat(NdotV), nl = mfloat(NdotL);
    mfloat Gh = nv / (nv * (mfloat(1.0) - kh) + kh) * (nl / (nl * (mfloat(1.0) - kh) + kh));
    mvec3 spec_h = Dh * Gh * Fh / (mfloat(4.0) * nv * nl + mfloat(1e-4));
    // The Lambert term is a few FP32 multiplies; in half it shifted large sunlit areas by one 8-bit
    // step (ADR-0153). Only the specular D, G and F terms, where FP16 saves registers, stay in half.
    vec3 kd_f = vec3((mvec3(1.0) - Fh) * (mfloat(1.0) - m));
    return (kd_f * albedo * (NdotL / PI) + vec3(spec_h * nl)) * radiance;
#endif
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), f0);
    float D = distribution_ggx(NdotH, roughness * roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3 spec = D * G * F / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + spec) * radiance * NdotL;
}

// ---- Clear coat (ADR-0203) ----
// A thin dielectric layer (IOR 1.5, F0 0.04) over the base, as in KHR_materials_clearcoat and the
// Filament / Unreal clear coat models: it has its own GGX lobe with its own roughness, uses the
// interpolated vertex normal (the coat is smooth over the base's normal map: lacquer over
// moulded detail), and the light it reflects no longer reaches the base, which is scaled by
// 1 - Fc. Returns the coat's reflected radiance factor (times N.L) in rgb and the base scale in a.
vec4 clear_coat(vec3 Nc, vec3 V, vec3 L, float coat, float coat_roughness) {
    float NdotL = max(dot(Nc, L), 0.0);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(Nc, V), 1e-4);
    float Fc = (0.04 + 0.96 * pow(clamp(1.0 - max(dot(H, V), 0.0), 0.0, 1.0), 5.0)) * coat;
    float D = distribution_ggx(max(dot(Nc, H), 0.0), coat_roughness * coat_roughness);
    // Kelemen visibility, as Filament uses for the coat: cheap and energy-bounded.
    float LdotH = max(dot(L, H), 1e-2);
    float Vis = 0.25 / (LdotH * LdotH);
    return vec4(vec3(D * Vis * Fc * NdotL), 1.0 - Fc);
}
vec3 with_coat(vec3 base_lit, vec3 Nc, vec3 V, vec3 L, vec3 radiance, float coat, float coat_roughness) {
    if (coat <= 0.0) return base_lit;
    vec4 k = clear_coat(Nc, V, L, coat, coat_roughness);
    return base_lit * k.a + k.rgb * radiance;
}

// ---- Anisotropic specular (ADR-0232) ----
// Brushed metal, hair, satin and machined parts stretch the highlight along one surface direction,
// as in KHR_materials_anisotropy and Filament: the roughness along the anisotropy direction t is
// raised to alpha_t = mix(alpha, 1, strength^2) while alpha_b = alpha across it (Burley's
// anisotropic GGX with the height-correlated anisotropic Smith visibility of Heitz 2014).
float distribution_ggx_aniso(float NdotH, float TdotH, float BdotH, float at, float ab) {
    float a2 = at * ab;
    vec3 v = vec3(ab * TdotH, at * BdotH, a2 * NdotH);
    float v2 = dot(v, v);
    float w2 = a2 / max(v2, 1e-7);
    return a2 * w2 * w2 / PI;
}
float visibility_ggx_aniso(float at, float ab, float TdotV, float BdotV, float TdotL, float BdotL, float NdotV, float NdotL) {
    float lambda_v = NdotL * length(vec3(at * TdotV, ab * BdotV, NdotV));
    float lambda_l = NdotV * length(vec3(at * TdotL, ab * BdotL, NdotL));
    return clamp(0.5 / max(lambda_v + lambda_l, 1e-6), 0.0, 1e4);
}
// brdf() with the anisotropic lobe; Ta / Ba are the anisotropy direction and its bitangent.
vec3 brdf_aniso(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness, vec3 Ta, vec3 Ba,
                float aniso) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float a = roughness * roughness;
    float at = mix(a, 1.0, aniso * aniso), ab = max(a, 1e-3);
    float D = distribution_ggx_aniso(NdotH, dot(Ta, H), dot(Ba, H), at, ab);
    float Vis = visibility_ggx_aniso(at, ab, dot(Ta, V), dot(Ba, V), dot(Ta, L), dot(Ba, L), NdotV, NdotL);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), f0);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + D * Vis * F) * radiance * NdotL;
}
// The isotropic brdf() when the material has no anisotropy (identical output, same cost).
vec3 brdf_any(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness, vec3 Ta, vec3 Ba,
              float aniso) {
    return aniso > 0.0 ? brdf_aniso(N, V, L, radiance, albedo, metallic, roughness, Ta, Ba, aniso)
                       : brdf(N, V, L, radiance, albedo, metallic, roughness);
}

// ---- Subsurface scattering (ADR-0233) ----
// Skin, wax, marble, jade, leaves, ears and candles: light enters, scatters and leaves elsewhere.
// Filament's subsurface model: the diffuse term wraps past the terminator (energy-conserving
// wrapped Lambert, w = 0.5) so the shadow edge softens and reddens, and a transmission lobe lets
// light through thin parts, strongest looking towards the light (forward scattering, power 12.234)
// and weaker as back scattering; both fade with thickness (1 = opaque, 0 = paper thin).
const float kSubsurfaceWrap = 0.5;
const float kSubsurfacePower = 12.234;
// Change to the base's Lambert term (times N.L) from wrapping it; negative on the lit side.
vec3 subsurface_wrap(vec3 N, vec3 L, vec3 radiance, vec3 albedo, float metallic) {
    float nl = dot(N, L);
    float wrapped = clamp((nl + kSubsurfaceWrap) / ((1.0 + kSubsurfaceWrap) * (1.0 + kSubsurfaceWrap)), 0.0, 1.0);
    return albedo * (1.0 - metallic) * ((wrapped - max(nl, 0.0)) / PI) * radiance;
}
// Light transmitted through the surface from L, tinted by the subsurface colour.
vec3 subsurface_transmission(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 sss_color, float thickness) {
    float scatter_voh = clamp(dot(V, -L), 0.0, 1.0);
    float forward = exp2(scatter_voh * kSubsurfacePower - kSubsurfacePower);
    float back = clamp(dot(N, L) * thickness + (1.0 - thickness), 0.0, 1.0) * 0.5;
    return sss_color * (mix(back, 1.0, forward) * (1.0 - thickness) / PI) * radiance;
}

// ---- Sheen (ADR-0231) ----
// Retro-reflective fuzz of cloth, velvet, felt, peach skin and dusty surfaces, as in
// KHR_materials_sheen and Filament's cloth model: the Charlie distribution (Estevez & Kulla 2017)
// with Neubelt's visibility term. The base below is scaled by 1 - max(sheen colour) * E(N.V), the
// sheen lobe's directional albedo (the glTF spec's albedo scaling), so the layer adds no energy.
float distribution_charlie(float NdotH, float roughness) {
    float inv_alpha = 1.0 / max(roughness * roughness, 1e-3);
    float sin2h = max(1.0 - NdotH * NdotH, 0.0078125); // 2^-7 avoids fp underflow
    return (2.0 + inv_alpha) * pow(sin2h, inv_alpha * 0.5) / (2.0 * PI);
}
float visibility_neubelt(float NdotV, float NdotL) {
    return clamp(1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV)), 0.0, 1.0);
}
// Directional albedo of the sheen lobe: the Imageworks analytic fit of the pre-integrated
// Charlie-Neubelt BRDF (also used for the ambient term).
float sheen_albedo(float NdotV, float roughness) {
    float r2 = roughness * roughness;
    float a = roughness < 0.25 ? -339.2 * r2 + 161.4 * roughness - 25.9 : -8.48 * r2 + 14.3 * roughness - 9.95;
    float b = roughness < 0.25 ? 44.0 * r2 - 23.7 * roughness + 3.26 : 1.97 * r2 - 3.27 * roughness + 0.72;
    float dg = exp(a * NdotV + b) + (roughness < 0.25 ? 0.0 : 0.1 * (roughness - 0.25));
    return clamp(dg / PI, 0.0, 1.0);
}
float max3(vec3 v) { return max(v.x, max(v.y, v.z)); }
// Base lighting from L with the sheen layer on top (N.L included, like brdf()).
vec3 with_sheen(vec3 base_lit, vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 sheen_color, float sheen_roughness,
                float base_scale) {
    if (max3(sheen_color) <= 0.0) return base_lit;
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    vec3 H = normalize(V + L);
    float lobe = distribution_charlie(max(dot(N, H), 0.0), sheen_roughness) * visibility_neubelt(NdotV, NdotL);
    return base_lit * base_scale + sheen_color * (lobe * NdotL) * radiance;
}

// ---- Area lights (ADR-0194) ----
// GGX specular lobe times N.L for light from L, with the distribution scaled by `norm`.
vec3 specular_lobe(vec3 N, vec3 V, vec3 L, vec3 f0, float roughness, float norm) {
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), f0);
    float D = distribution_ggx(max(dot(N, H), 0.0), roughness * roughness) * norm;
    float G = geometry_smith(NdotV, NdotL, roughness);
    return D * G * F / (4.0 * NdotV * NdotL + 1e-4) * NdotL;
}
// Lambert diffuse weight (without N.L) with the Fresnel and metallic split used by brdf().
vec3 diffuse_lobe(vec3 V, vec3 L, vec3 albedo, float metallic) {
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnel_schlick(max(dot(normalize(V + L), V), 0.0), f0);
    return (1.0 - F) * (1.0 - metallic) * albedo / PI;
}
// Point light with a spherical bulb of radius r: diffuse as a point (exact above the horizon),
// specular from the point of the sphere nearest the reflection ray with energy normalisation
// (Karis 2013, eq. 10, 11 and 14).
vec3 sphere_light(vec3 N, vec3 V, vec3 to_light, float r, vec3 radiance, vec3 albedo, float metallic, float roughness) {
    float dist = length(to_light);
    vec3 Ld = to_light / max(dist, 1e-4);
    vec3 R = reflect(-V, N);
    vec3 center_to_ray = dot(to_light, R) * R - to_light;
    vec3 closest = to_light + center_to_ray * clamp(r / max(length(center_to_ray), 1e-5), 0.0, 1.0);
    vec3 Ls = normalize(closest);
    float a = max(roughness * roughness, 1e-3);
    float a2 = clamp(a + r / (3.0 * max(dist, 1e-4)), 0.0, 1.0);
    float norm = (a / a2) * (a / a2);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    return (diffuse_lobe(V, Ld, albedo, metallic) * max(dot(N, Ld), 0.0) + specular_lobe(N, V, Ls, f0, roughness, norm)) * radiance;
}
// Same branchless basis as scene_renderer.cpp (Duff et al. 2017), so the CPU can send the width
// axis as one angle.
vec3 area_right(vec3 n, float angle) {
    float sgn = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sgn + n.z);
    float b = n.x * n.y * a;
    vec3 t = vec3(1.0 + sgn * n.x * n.x * a, sgn * b, -sgn * n.x);
    vec3 bt = vec3(b, sgn + n.y * n.y * a, -n.y);
    return t * cos(angle) + bt * sin(angle);
}
// Edge term of a polygon's vector form factor: acos(dot(a, b)) * normalize(cross(a, b)) / (2 pi),
// with Heitz et al.'s rational fit of theta / (2 pi sin(theta)) (LTC reference code, 2017).
vec3 form_factor_edge(vec3 a, vec3 b) {
    float x = dot(a, b);
    float y = abs(x);
    float u = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float v = 3.4175940 + (4.1616724 + y) * y;
    float t = u / v;
    float theta_sin = x > 0.0 ? t : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - t;
    return cross(a, b) * theta_sin;
}
// Clipping of a light partly below the horizon, approximated by a sphere with the same form
// factor (the "horizon cos wrap" used by UE4 rect lights); exact when fully above.
float horizon_wrap(float cos_n, float form_factor) {
    float sin_a = sqrt(clamp(form_factor, 0.0, 1.0));
    if (cos_n < sin_a) {
        cos_n = max(cos_n, -sin_a);
        cos_n = (sin_a + cos_n) * (sin_a + cos_n) / (4.0 * max(sin_a, 1e-4));
    }
    return cos_n;
}
// One-sided rectangle light (light_clusters.hpp layout). Diffuse: exact irradiance from the
// rectangle's vector form factor; specular: representative point where the reflection ray
// meets the rectangle (clamped to it), energy-normalised along each axis.
vec3 rect_light(LocalLight l, vec3 P, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness) {
    vec3 c = l.position_range.xyz;
    vec3 nl = normalize(l.direction_cos_outer.xyz);
    vec3 d = P - c;
    if (dot(d, nl) <= 1e-4) return vec3(0.0); // behind the emitting face
    vec3 ex = area_right(nl, l.direction_cos_outer.w);
    vec3 ey = cross(nl, ex);
    float hw = l.spot.x, hh = l.spot.w;
    vec2 q = clamp(vec2(dot(d, ex), dot(d, ey)), -vec2(hw, hh), vec2(hw, hh));
    float dist = length(c + ex * q.x + ey * q.y - P);
    float falloff = clamp(1.0 - pow(dist / l.position_range.w, 4.0), 0.0, 1.0);
    falloff *= falloff;
    if (falloff <= 0.0) return vec3(0.0);
    vec3 v0 = normalize(c - ex * hw - ey * hh - P);
    vec3 v1 = normalize(c + ex * hw - ey * hh - P);
    vec3 v2 = normalize(c + ex * hw + ey * hh - P);
    vec3 v3 = normalize(c - ex * hw + ey * hh - P);
    // Summed edge terms = vector form factor; times pi = integral of cos over the rectangle.
    vec3 F = PI * (form_factor_edge(v0, v1) + form_factor_edge(v1, v2) + form_factor_edge(v2, v3) + form_factor_edge(v3, v0));
    if (dot(F, c - P) < 0.0) F = -F; // winding seen from P
    float len = length(F); // irradiance on a surface facing the light, per unit radiance
    if (len <= 1e-7) return vec3(0.0);
    float E = len * horizon_wrap(dot(F, N) / len, len / PI);
    vec3 radiance = l.color_intensity.rgb * (l.color_intensity.w / (4.0 * hw * hh)) * falloff;
    vec3 Lc = normalize(c - P);
    vec3 color = diffuse_lobe(V, dot(N, Lc) > 0.0 ? Lc : N, albedo, metallic) * (E * radiance);
    // Specular representative point.
    vec3 R = reflect(-V, N);
    float denom = dot(R, nl);
    vec3 hit = denom < -1e-4 ? P + R * (dot(c - P, nl) / denom) : P + R * max(dot(c - P, R), 0.0);
    vec2 h = clamp(vec2(dot(hit - c, ex), dot(hit - c, ey)), -vec2(hw, hh), vec2(hw, hh));
    vec3 to_p = c + ex * h.x + ey * h.y - P;
    float ds = max(length(to_p), 1e-4);
    float a = max(roughness * roughness, 1e-3);
    float ax = clamp(a + hw / (3.0 * ds), 0.0, 1.0), ay = clamp(a + hh / (3.0 * ds), 0.0, 1.0);
    float norm = (a / ax) * (a / ay);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    color += specular_lobe(N, V, to_p / ds, f0, roughness, norm) * (len * radiance);
    return color;
}

float interleaved_gradient_noise(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

float sample_cascade(int c, vec3 N, vec3 L) {
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    vec3 offset = N * (lighting.cascade_texel[c] * lighting.shadow_params.z * (1.0 - ndl * 0.5));
    vec4 p4 = lighting.cascade_vp[c] * vec4(v_world_pos + offset, 1.0);
    vec3 p = p4.xyz / p4.w;
    vec2 uv = p.xy * vec2(0.5, -0.5) + 0.5;
    if (p.z > 1.0) return 1.0;
    float tile = lighting.cascade_params.y;
    vec2 origin = vec2(float(c & 1), float(c >> 1)) * tile;
    float texel = lighting.shadow_params.x;
    vec2 lo = origin + vec2(texel * 1.5), hi = origin + vec2(tile - texel * 1.5);
    vec2 auv = clamp(origin + uv * tile, lo, hi);
    float z = p.z - lighting.shadow_params.y;
    if (lighting.cascade_params.x < 0.5) return texture(u_shadow, vec3(auv, z));
    if (lighting.cascade_params.x > 1.5) {
        // Fast soft shadows: four hardware 2x2 PCF taps half a texel apart, a 3x3 tent filter for 4 fetches.
        vec2 h = vec2(texel * 0.5);
        return 0.25 * (texture(u_shadow, vec3(clamp(auv + vec2(-h.x, -h.y), lo, hi), z))
                     + texture(u_shadow, vec3(clamp(auv + vec2(h.x, -h.y), lo, hi), z))
                     + texture(u_shadow, vec3(clamp(auv + vec2(-h.x, h.y), lo, hi), z))
                     + texture(u_shadow, vec3(clamp(auv + vec2(h.x, h.y), lo, hi), z)));
    }
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(u_shadow, vec3(clamp(auv + vec2(x, y) * texel, lo, hi), z));
    return sum / 9.0;
}

// `baked`: the static sun visibility from the lightmap shadowmask (ADR-0131), 1 without one. It
// takes over where the realtime cascades end (Unity's distance shadowmask), so static geometry
// keeps its sun shadows at any distance; inside the cascades the realtime map covers every caster.
float shadow_factor(vec3 N, vec3 L, float baked) {
    if (lighting.sun_dir.w < 0.5) return baked;
    int count = int(lighting.shadow_params.w);
    float depth = dot(v_world_pos - lighting.camera_pos.xyz, lighting.camera_forward.xyz);
    int c = 0;
    while (c < count - 1 && depth > lighting.cascade_splits[c]) ++c;
    float far_d = lighting.cascade_splits[c];
    if (depth > far_d) return baked;
    float near_d = c == 0 ? 0.0 : lighting.cascade_splits[c - 1];
    float t = (depth - near_d) / max(far_d - near_d, 1e-4);
    float band = lighting.cascade_params.z;
    float fade = clamp((1.0 - t) / max(band, 1e-4), 0.0, 1.0); // 1 inside, 0 at the far edge
    if (c < count - 1) {
        // Dithered transition into the next cascade hides the resolution seam.
        if (interleaved_gradient_noise(gl_FragCoord.xy) > fade) c += 1;
        return sample_cascade(c, N, L);
    }
    return mix(baked, sample_cascade(c, N, L), fade); // last cascade fades out at shadow distance
}

// Cloud shadow (ADR-0112): transmittance of the sun ray through the cloud layer at its middle,
// from the first two octaves of the shape noise clouds.frag marches (same hash, lattice and
// octave rotation), so shadows sit under the clouds and drift with them.
float cloud_hash(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
// Puddles and rain ripples (ADR-0117). 2D value noise for where water collects, and the slope of
// expanding rings dropped at random times in a grid of cells (each cell holds one ring at a time).
float hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float puddle_noise(vec2 x) {
    vec2 i = floor(x), f = x - i;
    vec2 w = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + vec2(1, 0)), c = hash12(i + vec2(0, 1)), d = hash12(i + vec2(1, 1));
    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}
vec2 ripple_slope(vec2 p, float t) {
    vec2 cell = floor(p);
    vec2 slope = vec2(0.0);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            vec2 id = cell + vec2(i, j);
            vec2 centre = id + 0.2 + 0.6 * vec2(hash12(id), hash12(id + 17.31));
            float phase = fract(t + hash12(id + 5.7));
            vec2 d = p - centre;
            float dist = length(d);
            float x = (dist - phase * 0.9) / 0.12; // ring radius grows to 0.9 cells, 0.12 cells wide
            if (abs(x) < 1.0 && dist > 1e-4) {
                float fade = (1.0 - phase) * (1.0 - phase);
                slope += (d / dist) * (cos(x * 3.14159265) * (1.0 - x * x) * fade);
            }
        }
    return slope;
}

float cloud_noise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = x - i;
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(mix(cloud_hash(i), cloud_hash(i + vec3(1, 0, 0)), f.x), mix(cloud_hash(i + vec3(0, 1, 0)), cloud_hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(cloud_hash(i + vec3(0, 0, 1)), cloud_hash(i + vec3(1, 0, 1)), f.x), mix(cloud_hash(i + vec3(0, 1, 1)), cloud_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float cloud_shadow(vec3 L) {
    if (lighting.cloud_shadow.z < 0.5 || L.y < 0.02) return 1.0;
    float bottom = lighting.cloud_layer.x, top = lighting.cloud_layer.y;
    float mid = mix(bottom, top, 0.3); // where the height profile lets the most cloud through
    if (v_world_pos.y >= mid) return 1.0;
    vec3 q = (v_world_pos + L * ((mid - v_world_pos.y) / L.y) + lighting.cloud_wind.xyz) * lighting.cloud_shadow.x;
    const mat3 kOctave = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64) * 2.03;
    float base = (0.5 * cloud_noise(q) + 0.25 * cloud_noise(kOctave * q)) / 0.75;
    float thr = 1.0 - lighting.cloud_layer.z;
    float d = clamp((base - thr) / max(1.0 - thr, 0.05), 0.0, 1.0);
    // Optical depth of a vertical path through half the layer (the light march's view of a cloud).
    float od = d * lighting.cloud_layer.w * (top - bottom) * 0.5 / max(L.y, 0.2);
    return mix(1.0, exp(-od), lighting.cloud_shadow.y);
}

// Shadow of a local light: spot lights use one atlas tile, point lights six (one per cube face,
// +X -X +Y -Y +Z -Z, picked by the major axis of the light-to-surface vector).
// Spot light cookie (ADR-0195). spot.z = shadow tile + 1 + 4096 * (cookie record + 1); the record
// holds the rows of the cone's projection and the atlas slot. Cookies are packed sRGB RGBA8
// texels (SceneRenderer::kCookieSize squared per slot), filtered bilinearly here.
#define COOKIE_SIZE 256u
vec3 cookie_texel(uint base, uint x, uint y) {
    uint w = COOKIE(base + y * COOKIE_SIZE + x);
    vec3 c = vec3(uvec3(w, w >> 8u, w >> 16u) & 255u) / 255.0;
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); // sRGB -> linear
}
// Bilinear cookie fetch at uv in [0,1]^2; wrap_x / wrap_y repeat the image instead of clamping.
vec3 cookie_bilinear(uint slot, vec2 uv, bool wrap_x, bool wrap_y) {
    vec2 t = uv * float(COOKIE_SIZE) - 0.5;
    vec2 f = fract(t);
    ivec2 n = ivec2(int(COOKIE_SIZE));
    ivec2 i0 = ivec2(floor(t));
    ivec2 i1 = i0 + 1;
    ivec2 w0 = (i0 + n) % n, w1 = (i1 + n) % n; // i0 >= -1 for uv in [0, 1]
    ivec2 c0 = clamp(i0, ivec2(0), n - 1), c1 = clamp(i1, ivec2(0), n - 1);
    i0 = ivec2(wrap_x ? w0.x : c0.x, wrap_y ? w0.y : c0.y);
    i1 = ivec2(wrap_x ? w1.x : c1.x, wrap_y ? w1.y : c1.y);
    uint base = slot * COOKIE_SIZE * COOKIE_SIZE;
    vec3 a = mix(cookie_texel(base, uint(i0.x), uint(i0.y)), cookie_texel(base, uint(i1.x), uint(i0.y)), f.x);
    vec3 b = mix(cookie_texel(base, uint(i0.x), uint(i1.y)), cookie_texel(base, uint(i1.x), uint(i1.y)), f.x);
    return mix(a, b, f.y);
}
// Spot and point light cookies (ADR-0195, ADR-0201). A spot record projects through the cone; a
// point record (spot.y = 1) holds the light's right / up / forward axes and wraps the image
// around the light as a latitude-longitude panorama.
vec3 local_cookie(LocalLight l, vec3 P) {
    float record = floor((l.spot.z + 0.5) / 4096.0);
    if (record < 0.5) return vec3(1.0);
    LocalLight c = LOCAL_LIGHT(uint(record) - 1u);
    uint slot = uint(c.spot.x + 0.5);
    if (c.spot.y > 0.5) {
        vec3 d = P - l.position_range.xyz;
        float len = length(d);
        if (len < 1e-5) return vec3(1.0);
        d /= len;
        vec3 q = vec3(dot(d, c.position_range.xyz), dot(d, c.color_intensity.xyz), dot(d, c.direction_cos_outer.xyz));
        vec2 uv = vec2(0.5 + atan(q.x, q.z) * 0.15915494, acos(clamp(q.y, -1.0, 1.0)) * 0.31830989);
        return cookie_bilinear(slot, uv, true, false);
    }
    vec4 p = vec4(P, 1.0);
    float w = dot(c.direction_cos_outer, p);
    if (w <= 1e-5) return vec3(0.0);
    vec2 uv = clamp(vec2(dot(c.position_range, p), dot(c.color_intensity, p)) / w, 0.0, 1.0);
    return cookie_bilinear(slot, uv, false, false);
}
// Directional light cookie (ADR-0201): the image tiles across the plane facing the sun.
vec3 sun_cookie(vec3 P) {
    if (lighting.sun_cookie.x < 0.5) return vec3(1.0);
    vec3 d = normalize(lighting.sun_dir.xyz);
    vec3 r = lighting.sun_cookie_right.xyz;
    vec3 u = cross(r, d);
    vec2 uv = (vec2(dot(P, r), -dot(P, u)) + lighting.sun_cookie.zw) * lighting.sun_cookie.y;
    return cookie_bilinear(uint(lighting.sun_cookie.x - 0.5), fract(uv), true, true);
}

float local_shadow(LocalLight l, vec3 Ld, float dist, vec3 N) {
    float slot = mod(l.spot.z + 0.5, 4096.0) - 0.5;
    if (lighting.local_shadow.w < 0.5 || slot < 0.5) return 1.0;
    int tile = int(slot + 0.5) - 1;
    if (l.spot.y < 0.5) {
        vec3 d = -Ld;
        vec3 a = abs(d);
        int face = (a.x >= a.y && a.x >= a.z) ? (d.x >= 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y >= 0.0 ? 2 : 3) : (d.z >= 0.0 ? 4 : 5));
        tile += face;
    }
    LocalLight m = LOCAL_LIGHT(uint(lighting.local_shadow.z) + uint(tile));
    mat4 M = mat4(m.position_range, m.color_intensity, m.direction_cos_outer, m.spot);
    float ndl = clamp(dot(N, Ld), 0.0, 1.0);
    // Normal offset of about one shadow texel at this distance (texel angle * distance).
    vec3 offset = N * (dist * l.spot.w * 1.5 * (1.0 - ndl * 0.5));
    vec4 p4 = M * vec4(v_world_pos + offset, 1.0);
    if (p4.w <= 1e-5) return 1.0;
    vec3 p = p4.xyz / p4.w;
    if (p.z >= 1.0) return 1.0;
    float n = lighting.local_shadow.y;
    float scale = 1.0 / n;
    vec2 origin = vec2(mod(float(tile), n), floor(float(tile) / n)) * scale;
    float texel = lighting.local_shadow.x;
    vec2 lo = origin + vec2(texel * 1.5), hi = origin + vec2(scale - texel * 1.5);
    vec2 auv = clamp(p.xy, lo, hi);
    if (lighting.cascade_params.x < 0.5) return texture(u_local_shadow, vec3(auv, p.z));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) sum += texture(u_local_shadow, vec3(clamp(auv + vec2(x, y) * texel, lo, hi), p.z));
    return sum / 9.0;
}

// World-space triplanar mapping (ADR-0197). Each texture is projected along the three world axes
// and the projections are blended by the surface normal, so scaled boxes, terrain-like blocks and
// meshes without good UVs tile evenly at uv_transform.xy repeats per metre. The projected u axis
// is flipped on faces that look down a negative axis so no face shows a mirrored image, and normal
// maps are reoriented with the "whiteout" blend (Golus, "Normal Mapping for a Triplanar Shader").
struct Triplanar { vec2 uv_x, uv_y, uv_z; vec3 w, s; };
Triplanar triplanar_setup(vec3 n) {
    Triplanar t;
    vec3 p = v_world_pos;
    t.s = vec3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
    vec2 sc = material.uv_transform.xy, of = material.uv_transform.zw;
    t.uv_x = vec2(-p.z * t.s.x, -p.y) * sc + of;
    t.uv_y = vec2(-p.x * t.s.y, -p.z) * sc + of;
    t.uv_z = vec2(p.x * t.s.z, -p.y) * sc + of;
    vec3 w = pow(abs(n), vec3(max(material.mapping.y, 1.0)));
    t.w = w / max(w.x + w.y + w.z, 1e-5);
    return t;
}
vec4 triplanar_sample(sampler2D tex, Triplanar t) {
    float bias = lighting.camera_forward.w;
    return texture(tex, t.uv_x, bias) * t.w.x + texture(tex, t.uv_y, bias) * t.w.y + texture(tex, t.uv_z, bias) * t.w.z;
}
vec3 triplanar_normal(Triplanar t, vec3 n) {
    float bias = lighting.camera_forward.w;
    vec3 tx = texture(u_normal_map, t.uv_x, bias).xyz * 2.0 - 1.0;
    vec3 ty = texture(u_normal_map, t.uv_y, bias).xyz * 2.0 - 1.0;
    vec3 tz = texture(u_normal_map, t.uv_z, bias).xyz * 2.0 - 1.0;
    tx.xy *= material.params.z;
    ty.xy *= material.params.z;
    tz.xy *= material.params.z;
    tx.x *= -t.s.x;
    ty.x *= -t.s.y;
    tz.x *= t.s.z;
    tx = vec3(tx.xy + n.zy, abs(tx.z) * n.x);
    ty = vec3(ty.xy + n.xz, abs(ty.z) * n.y);
    tz = vec3(tz.xy + n.xy, abs(tz.z) * n.z);
    return normalize(tx.zyx * t.w.x + ty.xzy * t.w.y + tz.xyz * t.w.z);
}

void main() {
    vec2 uv = v_uv * material.uv_transform.xy + material.uv_transform.zw;
    bool triplanar = material.mapping.x > 0.5;
    vec3 geometric_n = normalize(v_normal);
    if (!gl_FrontFacing) geometric_n = -geometric_n;
    Triplanar tp = triplanar_setup(geometric_n);
    vec4 base = (triplanar ? triplanar_sample(u_base_color, tp) : texture(u_base_color, uv, lighting.camera_forward.w)) *
                material.base_color;
    bool alpha_is_mask = material.flags.z > 0.5;
    Surface s;
    s.base_color = base.rgb * v_color.rgb;
    s.alpha = alpha_is_mask ? base.a : base.a * v_color.a;
    s.emissive = (triplanar ? triplanar_sample(u_emissive, tp) : texture(u_emissive, uv, lighting.camera_forward.w)).rgb *
                 material.emissive.rgb;
    s.unlit = material.flags.y > 0.5;
    vec4 mr = triplanar ? triplanar_sample(u_metal_rough, tp) : texture(u_metal_rough, uv, lighting.camera_forward.w);
    s.occlusion = 1.0 + material.blend.y * (mr.r - 1.0); // glTF ORM: red = ambient occlusion
    s.metallic = clamp(material.params.x * mr.b, 0.0, 1.0);
    s.roughness = clamp(material.params.y * mr.g, 0.045, 1.0);
    vec3 N = geometric_n;
    if (material.flags.x > 0.5) {
        if (triplanar) {
            N = triplanar_normal(tp, geometric_n);
        } else {
            vec3 T = normalize(v_tangent.xyz - N * dot(N, v_tangent.xyz));
            vec3 B = cross(N, T) * v_tangent.w;
            vec3 tn = texture(u_normal_map, uv, lighting.camera_forward.w).xyz * 2.0 - 1.0;
            tn.xy *= material.params.z;
            N = normalize(mat3(T, B, N) * tn);
        }
    }
    s.normal = N;
    vec3 V = normalize(lighting.camera_pos.xyz - v_world_pos);
#ifdef VICTUS_SURFACE_SHADER
    SurfaceInput si;
    si.uv = uv;
    si.mesh_uv = v_uv;
    si.world_position = v_world_pos;
    si.view_dir = V;
    si.vertex_normal = geometric_n;
    si.time = lighting.screen.z;
    si.params = material.custom;
    si.vertex_color = v_color;
    surface(si, s);
    s.normal = normalize(s.normal);
    s.metallic = clamp(s.metallic, 0.0, 1.0);
    s.roughness = clamp(s.roughness, 0.045, 1.0);
#endif
    if (material.blend.z > 0.5) {
        // Alpha to coverage (ADR-0152): the cut-out edge becomes a one-pixel coverage ramp that
        // the MSAA samples resolve, instead of a hard per-pixel discard that shimmers.
        float a2c_width = max(fwidth(s.alpha), 1e-4);
        s.alpha = clamp((s.alpha - material.params.w) / a2c_width + 0.5, 0.0, 1.0);
        if (s.alpha <= 0.0) discard;
    } else if (s.alpha < material.params.w) {
        discard;
    }
    if (material.blend.w != 0.0) {
        // LOD cross-fade (ADR-0184): the incoming and outgoing levels keep complementary pixels
        // of one screen-door pattern, so together they cover every pixel exactly once.
        float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if ((dither < abs(material.blend.w)) != (material.blend.w > 0.0)) discard;
    }
    if (s.unlit) {
        out_color = vec4(blended_fog(s.base_color + s.emissive), s.alpha);
        if (lighting.screen.w > 0.0) out_color.rgb = hdr_quantize(out_color.rgb, lighting.screen.w);
        return;
    }
    N = s.normal;
    vec3 albedo = s.base_color;
    float metallic = s.metallic;
    float roughness = s.roughness;

    // Wet surfaces (Lagarde, "Water drop 3b", 2013): porous dielectrics (here: rough, non-metal)
    // darken towards 0.2x albedo and get glossier; on faces that point up a water film makes any
    // surface smoother. Applied before the coverage layer, which settles on top.
    float wet = lighting.weather.x;
    float puddle_amount = lighting.weather.w, ripple = lighting.puddles.y;
    if ((wet > 0.0 || puddle_amount > 0.0 || ripple > 0.0) && lighting.weather.y > 0.5) { // sheltered from the rain: stays dry
        vec4 r4 = lighting.rain_vp * vec4(v_world_pos + geometric_n * 0.05, 1.0);
        vec3 r = r4.xyz / r4.w;
        vec2 ruv = r.xy * vec2(0.5, -0.5) + 0.5;
        if (all(greaterThanEqual(ruv, vec2(0.0))) && all(lessThanEqual(ruv, vec2(1.0))) && r.z <= 1.0) {
            float exposed = texture(u_rain_occlusion, vec3(ruv, r.z - lighting.weather.z));
            wet *= exposed;
            puddle_amount *= exposed;
            ripple *= exposed;
        }
    }
    // Puddles collect on flat, upward faces where the noise is above a threshold set by the amount;
    // inside them the surface is fully wet with a flat, mirror-smooth water top.
    float flat_up = clamp((geometric_n.y - 0.85) / 0.1, 0.0, 1.0);
    float puddle = 0.0;
    if (puddle_amount > 0.0 && flat_up > 0.0) {
        vec2 q = v_world_pos.xz * lighting.puddles.x;
        float n = 0.65 * puddle_noise(q) + 0.35 * puddle_noise(q * 2.7 + 13.1);
        float threshold = 1.0 - puddle_amount;
        puddle = smoothstep(threshold - 0.04, threshold + 0.04, n) * flat_up;
        wet = max(wet, puddle);
    }
    if (puddle > 0.0) N = normalize(mix(N, geometric_n, puddle));
    // Rain ripples on puddles (full strength) and, weaker, on the wet film of other flat faces.
    // They fade out where a pixel covers more than about a third of a ripple cell (no sparkle);
    // the derivative is taken in uniform control flow.
    vec2 rp = v_world_pos.xz * lighting.puddles.z;
    float footprint = length(fwidth(rp));
    if (ripple > 0.0 && flat_up > 0.0) {
        float k = ripple * flat_up * mix(0.25 * wet, 1.0, puddle) * clamp(2.0 - footprint * 3.0, 0.0, 1.0);
        if (k > 0.0) {
            vec2 g = ripple_slope(rp, lighting.screen.z * lighting.puddles.w);
            N = normalize(N - vec3(g.x, 0.0, g.y) * (0.6 * k));
        }
    }
    if (wet > 0.0) {
        float porosity = clamp((roughness - 0.2) / 0.6, 0.0, 1.0) * (1.0 - metallic);
        float factor = mix(1.0, 0.2, porosity);
        albedo *= mix(1.0, factor, wet);
        roughness *= mix(1.0, factor, 0.5 * wet);
        float up = clamp(N.y, 0.0, 1.0);
        roughness = clamp(mix(roughness, min(roughness, 0.08), wet * up * up), 0.045, 1.0);
        roughness = mix(roughness, 0.045, puddle); // standing water
    }

    // Coverage layer: settles on faces that point up, scaled by the scene amount, the material's
    // response and (optionally) the vertex alpha mask.
    float coverage = lighting.coverage_color.w * material.flags.w;
    if (coverage > 0.0) {
        float up = clamp((N.y - lighting.coverage_params.y) * lighting.coverage_params.z, 0.0, 1.0);
        float mask = alpha_is_mask ? step(0.5, v_color.a) : 1.0;
        coverage = clamp(coverage * up * mask, 0.0, 1.0);
        albedo = mix(albedo, lighting.coverage_color.rgb, coverage);
        roughness = mix(roughness, lighting.coverage_params.x, coverage);
        metallic = mix(metallic, 0.0, coverage);
    }

    // Clear coat: strength 0..1 and its own roughness; the coat follows the vertex normal and
    // dries off nothing (wet and coverage layers act on the base below it).
    float coat = clamp(material.layers.x, 0.0, 1.0);
    float coat_roughness = clamp(material.layers.y, 0.045, 1.0);
    vec3 Nc = geometric_n;
    // Sheen: colour 0 = none; it follows the shading normal (fibres follow the weave's normal map).
    vec3 sheen_color = max(material.sheen.rgb, vec3(0.0));
    float sheen_roughness = clamp(material.sheen.a, 0.07, 1.0);
    float sheen_scale = 1.0 - max3(sheen_color) * sheen_albedo(max(dot(N, V), 1e-4), sheen_roughness);
    // Anisotropy: the direction is the mesh tangent rotated about the shading normal (meshes
    // without tangents fall back to the horizontal direction around the normal).
    // Subsurface: colour 0 = none.
    vec3 sss_color = max(material.subsurface.rgb, vec3(0.0));
    float sss_thickness = clamp(material.subsurface.a, 0.0, 1.0);
    bool sss = max3(sss_color) > 0.0;
    float aniso = clamp(material.anisotropy.x, 0.0, 1.0);
    vec3 Ta = vec3(1.0, 0.0, 0.0), Ba = vec3(0.0, 0.0, 1.0);
    if (aniso > 0.0) {
        vec3 t = v_tangent.xyz - N * dot(N, v_tangent.xyz);
        if (dot(t, t) < 1e-6) t = cross(abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), N);
        t = normalize(t);
        vec3 b = cross(N, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0);
        float cr = cos(material.anisotropy.y), sr = sin(material.anisotropy.y);
        Ta = normalize(t * cr + b * sr);
        Ba = normalize(cross(N, Ta));
    }

    vec3 L = normalize(-lighting.sun_dir.xyz);
    vec4 lm_texel = v_lightmap.z > 0.5 ? texture(u_lightmap, v_lightmap.xy) : vec4(0.0, 0.0, 0.0, 1.0);
    // Ambient for unlightmapped surfaces comes from the probe volume, which also carries the baked
    // sun visibility (probe sun occlusion, ADR-0136): dynamic objects past the realtime shadow
    // distance stay shadowed by static geometry, as lightmapped ones do through the shadowmask.
    vec3 sky_irradiance = env_irradiance(N);
    float probe_sun = 1.0;
    vec3 probe_irradiance = v_lightmap.z > 0.5 ? sky_irradiance : volume_irradiance(v_world_pos, N, sky_irradiance, probe_sun);
    float baked_sun = lighting.cascade_params.w > 0.5 ? (v_lightmap.z > 0.5 ? lm_texel.a : probe_sun) : 1.0;
    vec2 screen_occlusion = lighting.camera_pos.w > 0.5 ? texture(u_ao, gl_FragCoord.xy * lighting.screen.xy).rg : vec2(1.0);
    float contact = lighting.camera_pos.w > 1.5 ? screen_occlusion.g : 1.0;
    float sun_visibility = shadow_factor(N, L, baked_sun) * cloud_shadow(L) * contact;
    vec3 sun_radiance = lighting.sun_color.rgb * sun_cookie(v_world_pos);
    vec3 color = with_coat(with_sheen(brdf_any(N, V, L, sun_radiance, albedo, metallic, roughness, Ta, Ba, aniso), N, V, L, sun_radiance,
                                      sheen_color, sheen_roughness, sheen_scale),
                           Nc, V, L, sun_radiance, coat, coat_roughness) * sun_visibility;
    if (sss) {
        // The wrapped diffuse shares the surface's shadow; the transmitted light is shadowed
        // as seen from the far side (normal offset along -N), so thin leaves and ears pass
        // sunlight while a thick body still blocks it.
        color += subsurface_wrap(N, L, sun_radiance, albedo, metallic) * sun_visibility;
        float through = shadow_factor(-N, L, baked_sun) * cloud_shadow(L);
        color += subsurface_transmission(N, V, L, sun_radiance, sss_color, sss_thickness) * through;
    }

    if (lighting.ground_color.w > 0.5) {
        vec3 grid = lighting.cluster_grid.xyz;
        vec2 tile = clamp(floor(gl_FragCoord.xy * lighting.screen.xy * grid.xy), vec2(0.0), grid.xy - 1.0);
        float view_depth = max(dot(v_world_pos - lighting.camera_pos.xyz, lighting.camera_forward.xyz), lighting.cluster_depth.x);
        float slice = clamp(floor(log(view_depth / lighting.cluster_depth.x) * lighting.cluster_depth.y), 0.0, grid.z - 1.0);
        uint cluster = uint((slice * grid.y + tile.y) * grid.x + tile.x);
        uint first = CLUSTER(2u * cluster), count = min(CLUSTER(2u * cluster + 1u), uint(lighting.cluster_grid.w));
        for (uint i = 0u; i < count; ++i) {
            LocalLight l = LOCAL_LIGHT(CLUSTER(first + i));
            if (l.spot.y > 1.5) {
                color += rect_light(l, v_world_pos, N, V, albedo, metallic, roughness);
                continue;
            }
            vec3 to_light = l.position_range.xyz - v_world_pos;
            float dist = length(to_light);
            float range = l.position_range.w;
            float falloff = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
            float atten = falloff * falloff / (dist * dist + 1.0);
            vec3 Ld = to_light / max(dist, 1e-4);
            if (l.spot.y > 0.5) { // smooth cone: 1 inside the inner angle, 0 outside the outer
                float cd = dot(-Ld, l.direction_cos_outer.xyz);
                float t = clamp((cd - l.direction_cos_outer.w) / max(l.spot.x - l.direction_cos_outer.w, 1e-4), 0.0, 1.0);
                atten *= t * t;
            }
            if (atten <= 0.0) continue;
            atten *= local_shadow(l, Ld, dist, N);
            if (atten <= 0.0) continue;
            vec3 radiance = l.color_intensity.rgb * l.color_intensity.w * atten;
            if (l.spot.y < 1.5) radiance *= local_cookie(l, v_world_pos);
            vec3 lit = (l.spot.y < 0.5 && l.spot.x > 0.0) ? sphere_light(N, V, to_light, l.spot.x, radiance, albedo, metallic, roughness)
                                                          : brdf_any(N, V, Ld, radiance, albedo, metallic, roughness, Ta, Ba, aniso);
            lit = with_sheen(lit, N, V, Ld, radiance, sheen_color, sheen_roughness, sheen_scale);
            if (sss)
                lit += subsurface_wrap(N, Ld, radiance, albedo, metallic) +
                       subsurface_transmission(N, V, Ld, radiance, sss_color, sss_thickness);
            color += with_coat(lit, Nc, V, Ld, radiance, coat, coat_roughness);
        }
    }

    float NdotV = max(dot(N, V), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 kd = (1.0 - fresnel_schlick(NdotV, f0)) * (1.0 - metallic);
    vec3 irradiance;
    float lightmap_occlusion = 1.0;
    if (v_lightmap.z > 0.5) {
        // Baked sky light and bounces replace the probe ambient. Where the bake is darker than
        // the open sky (under tables, in corners) the environment reflection is dimmed too.
        irradiance = lm_texel.rgb;
        // Directional lightmap: re-light the baked irradiance with the per-pixel normal (normal
        // maps), scaled so the normal it was baked with gets exactly the stored value.
        vec4 lm_dir = texture(u_lightmap_dir, v_lightmap.xy);
        if (lm_dir.a >= 0.25)
            irradiance *= (0.5 * dot(N, lm_dir.xyz * 2.0 - 1.0) + 0.5) / lm_dir.a;
        float sky_l = dot(sky_irradiance, vec3(0.2126, 0.7152, 0.0722));
        float lm_l = dot(irradiance, vec3(0.2126, 0.7152, 0.0722));
        lightmap_occlusion = sky_l > 1e-4 ? clamp(lm_l / sky_l, 0.0, 1.0) : 1.0;
    } else {
        irradiance = probe_irradiance;
    }
    vec2 ab = env_brdf(NdotV, roughness);
    vec3 R = reflect(-V, N);
    if (aniso > 0.0) {
        // Bent reflection vector (KHR_materials_anisotropy reference): the environment is sampled
        // along the normal bent towards the plane across the anisotropy direction, so brushed
        // metal reflects a streak instead of a sharp image.
        vec3 bent = normalize(cross(cross(Ba, V), Ba));
        float k = 1.0 - aniso * (1.0 - roughness);
        k *= k;
        R = reflect(-V, normalize(mix(bent, N, k * k)));
    }
    // Specular occlusion approximation: reflections below the local horizon are dimmed.
    float horizon = clamp(1.0 + dot(R, geometric_n), 0.0, 1.0);
    vec3 env_radiance = env_specular(R, roughness, v_world_pos);
#ifdef VX_SSR
    vec4 ssr = ssr_trace(v_world_pos, R, geometric_n, roughness);
    env_radiance = mix(env_radiance, ssr.rgb, ssr.a);
#endif
    vec3 specular = env_radiance * (f0 * ab.x + ab.y) * horizon * horizon;
    float ao = screen_occlusion.r;
    ao *= s.occlusion;
    // Specular occlusion from AO (Lagarde & de Rousiers 2014 approximation).
    float spec_ao = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    spec_ao *= lightmap_occlusion;
    vec3 ambient = kd * albedo * irradiance * ao + specular * spec_ao;
    if (max3(sheen_color) > 0.0) {
        // Ambient sheen: the lobe is broad, so the diffuse irradiance stands in for the
        // pre-filtered environment, weighted by the lobe's directional albedo.
        float sheen_dg = sheen_albedo(NdotV, sheen_roughness);
        ambient = ambient * sheen_scale + sheen_color * irradiance * (sheen_dg * ao * lightmap_occlusion);
    }
    if (sss) {
        // Ambient transmission (Filament's subsurface IBL): the diffuse irradiance plus the
        // blurred environment behind the surface, seen through it.
        vec3 behind = env_specular(-V, 1.0, v_world_pos);
        ambient += sss_color * (irradiance + behind) * ((1.0 - sss_thickness) / (2.0 * PI)) * ao * lightmap_occlusion;
    }
    if (coat > 0.0) {
        // The coat's environment reflection along the vertex normal; it takes the screen-space
        // reflection when the base traced one (close enough: the base normal is normal-mapped).
        float NdotVc = max(dot(Nc, V), 1e-4);
        vec2 abc = env_brdf(NdotVc, coat_roughness);
        float Fcv = (0.04 * abc.x + abc.y) * coat;
        vec3 Rc = reflect(-V, Nc);
        vec3 coat_env = env_specular(Rc, coat_roughness, v_world_pos);
#ifdef VX_SSR
        coat_env = mix(coat_env, ssr.rgb, ssr.a);
#endif
        float coat_ao = clamp(pow(NdotVc + ao, exp2(-16.0 * coat_roughness - 1.0)) - 1.0 + ao, 0.0, 1.0) * lightmap_occlusion;
        float coat_horizon = clamp(1.0 + dot(Rc, geometric_n), 0.0, 1.0);
        ambient = ambient * (1.0 - Fcv) + coat_env * Fcv * coat_ao * coat_horizon;
    }
    color += ambient;
    color += s.emissive;

    int fog_mode = int(lighting.fog_color.w + 0.5);
    if (fog_mode > 0) {
        float d = length(v_world_pos - lighting.camera_pos.xyz);
        float f;
        if (fog_mode == 1) f = clamp((d - lighting.fog_params.x) / max(lighting.fog_params.y - lighting.fog_params.x, 1e-3), 0.0, 1.0);
        else {
            float k = max(d - lighting.fog_params.x, 0.0) * lighting.fog_params.z;
            f = fog_mode == 2 ? 1.0 - exp(-k) : 1.0 - exp(-k * k);
        }
        color = mix(color, lighting.fog_color.rgb, f * lighting.fog_params.w);
    }
    out_color = vec4(blended_fog(color), s.alpha);
    if (lighting.screen.w > 0.0) out_color.rgb = hdr_quantize(out_color.rgb, lighting.screen.w);
}
