#version 450
// SPDX-License-Identifier: Apache-2.0
// Volumetric clouds, pass 1 of 2 (ADR-0111): ray marches a horizontal cloud layer at reduced
// resolution. Density is procedural (value-noise fBm shape thresholded by coverage and a height
// profile, eroded by a finer detail fBm; Schneider & Vos 2015), lit by the sun through a short
// light march (Beer-Lambert with Wrenninge-style multiple-scattering octaves and a dual-lobe
// Henyey-Greenstein phase) plus height-shaded sky ambient, and integrated with Hillaire's
// energy-conserving step (2015). Output: rgb in-scattered light, a = 1 - transmittance. Each
// cloud pixel marches up to the farthest scene depth of its footprint; geometry nearer than the
// layer is masked at full resolution by clouds_apply.frag.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Clouds {
    mat4 inv_view_proj;
    vec4 camera;    // xyz world position, w: max march distance (metres)
    vec4 sun_dir;   // xyz towards the sun, w: anisotropy g
    vec4 sun_color; // rgb sun radiance scaled by the cloud sun multiplier, w: light steps
    vec4 ambient;   // rgb mean sky radiance scaled by the ambient multiplier, w: primary steps
    vec4 layer;     // x bottom altitude, y top altitude, z coverage 0..1, w extinction per metre
    vec4 noise;     // x 1 / shape size, y 1 / detail size, z detail erosion 0..1, w jitter frame
    vec4 wind;      // xyz shape offset (metres), w: unused
    vec4 screen;    // xy: 1 / cloud target size, zw: half a cloud texel in uv (depth footprint)
} u;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float value_noise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = x - i;
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n000 = hash13(i), n100 = hash13(i + vec3(1, 0, 0));
    float n010 = hash13(i + vec3(0, 1, 0)), n110 = hash13(i + vec3(1, 1, 0));
    float n001 = hash13(i + vec3(0, 0, 1)), n101 = hash13(i + vec3(1, 0, 1));
    float n011 = hash13(i + vec3(0, 1, 1)), n111 = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y), mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// Octaves are rotated against each other so the lattice does not line up.
const mat3 kOctave = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64) * 2.03;

float fbm(vec3 p, int octaves) {
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * value_noise(p);
        norm += amp;
        amp *= 0.5;
        p = kOctave * p;
    }
    return sum / norm;
}

float layer_height(float y) { return (y - u.layer.x) / max(u.layer.y - u.layer.x, 1.0); }

// Extinction per metre at p (0 outside the layer).
float density(vec3 p, bool detail) {
    float h = layer_height(p.y);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    // Rounded bottoms and tops: the coverage threshold rises towards both boundaries.
    float profile = smoothstep(0.0, 0.15, h) * (1.0 - smoothstep(0.45, 1.0, h));
    float thr = 1.0 - u.layer.z * profile;
    if (thr >= 1.0) return 0.0;
    float base = fbm((p + u.wind.xyz) * u.noise.x, 4);
    float d = clamp((base - thr) / max(1.0 - thr, 0.05), 0.0, 1.0);
    if (d <= 0.0) return 0.0;
    if (detail && u.noise.z > 0.0) {
        float n = fbm((p + u.wind.xyz * 1.5) * u.noise.y, 2);
        d = clamp(d - u.noise.z * n * (1.0 - d), 0.0, 1.0);
    }
    return d * u.layer.w;
}

float hg(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

vec3 world_at(vec2 uv, float d) {
    vec4 p = u.inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    return p.xyz / p.w;
}

void main() {
    vec2 uv = gl_FragCoord.xy * u.screen.xy;
    vec3 o = u.camera.xyz;
    vec3 far_p = world_at(uv, 1.0);
    vec3 dir = normalize(far_p - world_at(uv, 0.0));
    // Farthest scene depth over the footprint of this cloud pixel.
    vec2 hpx = u.screen.zw;
    float dmax = max(max(textureLod(u_depth, uv + vec2(-hpx.x, -hpx.y), 0.0).r, textureLod(u_depth, uv + vec2(hpx.x, -hpx.y), 0.0).r),
                     max(textureLod(u_depth, uv + vec2(-hpx.x, hpx.y), 0.0).r, textureLod(u_depth, uv + vec2(hpx.x, hpx.y), 0.0).r));
    float t_scene = dmax >= 1.0 ? 1e30 : length(world_at(uv, dmax) - o);
    float max_t = min(u.camera.w, t_scene);

    float t0, t1;
    if (abs(dir.y) < 1e-5) {
        if (o.y <= u.layer.x || o.y >= u.layer.y) { out_color = vec4(0.0); return; }
        t0 = 0.0;
        t1 = max_t;
    } else {
        float ta = (u.layer.x - o.y) / dir.y, tb = (u.layer.y - o.y) / dir.y;
        t0 = max(min(ta, tb), 0.0);
        t1 = min(max(ta, tb), max_t);
    }
    if (t1 <= t0) { out_color = vec4(0.0); return; }

    int steps = int(u.ambient.w), light_steps = int(u.sun_color.w);
    float dt = (t1 - t0) / float(steps);
    // Interleaved gradient noise (Jimenez 2014), shifted per frame so TAA averages the banding.
    vec2 px = gl_FragCoord.xy + u.noise.w * 5.588238;
    float jitter = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
    vec3 L = normalize(u.sun_dir.xyz);
    float cos_theta = dot(dir, L);
    float g = u.sun_dir.w;
    float thickness = u.layer.y - u.layer.x;
    float light_len = min((u.layer.y - u.layer.x) / max(L.y, 0.1), thickness * 3.0);
    float light_dt = light_len / float(max(light_steps, 1));
    bool lit = dot(u.sun_color.rgb, vec3(1.0)) > 0.0 && light_steps > 0;

    vec3 scattered = vec3(0.0);
    float T = 1.0;
    for (int i = 0; i < steps; ++i) {
        float t = t0 + (float(i) + jitter) * dt;
        vec3 p = o + dir * t;
        float s = density(p, true);
        // Fade towards the max distance so the layer ends softly at the horizon.
        s *= 1.0 - smoothstep(0.6 * u.camera.w, u.camera.w, t);
        if (s <= 0.0) continue;
        vec3 S = u.ambient.rgb * mix(0.5, 1.0, clamp(layer_height(p.y), 0.0, 1.0));
        if (lit) {
            float od = 0.0;
            for (int j = 0; j < light_steps; ++j) od += density(p + L * ((float(j) + 0.5) * light_dt), false) * light_dt;
            // Multiple scattering approximation: octaves with weaker extinction and a softer phase.
            float a = 1.0, b = 1.0, c = 1.0;
            vec3 sun = vec3(0.0);
            for (int k = 0; k < 3; ++k) {
                float phase = mix(hg(cos_theta, g * c), hg(cos_theta, -0.3 * c), 0.25);
                sun += u.sun_color.rgb * (a * phase * exp(-od * b));
                a *= 0.5;
                b *= 0.3;
                c *= 0.5;
            }
            S += sun;
        }
        // Energy-conserving integration of in-scattering over the step (albedo 1: sigma_s = sigma_t).
        float step_t = exp(-s * dt);
        scattered += T * S * (1.0 - step_t);
        T *= step_t;
        if (T < 0.01) break;
    }
    out_color = vec4(scattered, 1.0 - T);
}
