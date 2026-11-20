#version 450
// SPDX-License-Identifier: Apache-2.0
// Water shading (ADR-0095): analytic Gerstner normals plus distance-faded ripples, Schlick
// Fresnel between an environment reflection and the refracted scene, Beer-Lambert absorption
// over the water thickness reconstructed from scene depth, GGX sun glint, shoreline and crest
// foam, the scene's distance fog and a soft edge where the surface meets geometry. The pass has
// no depth attachment: it tests against sampled opaque depth itself (water depth is written by
// a following depth-only pass).
layout(set = 2, binding = 0) uniform sampler2D u_scene; // copy of the HDR colour before water
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 2, binding = 2) uniform samplerCube u_env;
layout(set = 3, binding = 0) uniform Params {
    mat4 inv_view_proj; // jittered, matches the depth buffer
    vec4 camera;        // xyz position, w time (s)
    vec4 sun_dir;       // xyz towards the sun
    vec4 sun_color;     // rgb radiance
    vec4 shallow;       // rgb transmission tint, w clarity (m)
    vec4 deep;          // rgb water body colour, w foam amount
    vec4 params;        // x refraction, y roughness, z environment map present, w its max mip
    vec4 env;           // x intensity, y rotation (rad), z rest height
    vec4 sky;           // rgb zenith radiance
    vec4 horizon;       // rgb horizon radiance
    vec4 ground;        // rgb ground radiance
    vec4 screen;        // xy 1 / render size
    vec4 fog_color;     // rgb, w mode
    vec4 fog_params;    // x start, y end, z density, w max opacity
    vec4 wave_a[4];
    vec4 wave_b[4];
} u;
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec2 v_rest;
layout(location = 0) out vec4 out_color;
// VICTUS_REACTIVE_VARIANT: the "#reactive" build also writes the TAA reactive mask (ADR-0143), an
// R8 second target blended like the colour: how much the resolve should trust this frame here.
#ifdef VICTUS_REACTIVE
layout(location = 1) out vec4 out_reactive;
#endif

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float value_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 w = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), w.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), w.x), w.y);
}
vec3 world_at(vec2 suv, float depth) {
    vec4 wp = u.inv_view_proj * vec4(suv.x * 2.0 - 1.0, 1.0 - suv.y * 2.0, depth, 1.0);
    return wp.xyz / wp.w;
}
vec3 sky_radiance(vec3 dir, float roughness) {
    if (u.params.z > 0.5) {
        float c = cos(u.env.y), s = sin(u.env.y);
        vec3 d = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
        return textureLod(u_env, d, roughness * u.params.w).rgb * u.env.x;
    }
    float h = clamp(dir.y, -1.0, 1.0);
    return h > 0.0 ? mix(u.horizon.rgb, u.sky.rgb, sqrt(h)) : mix(u.horizon.rgb, u.ground.rgb, pow(-h, 0.4));
}

void main() {
    vec2 suv = gl_FragCoord.xy * u.screen.xy;
    float scene_depth = textureLod(u_depth, suv, 0.0).r;
    if (gl_FragCoord.z > scene_depth) discard; // behind opaque geometry

    // Normal: the Gerstner waves analytically (GPU Gems 1 ch. 1, eq. 12), plus three ripples.
    vec3 n = vec3(0.0, 1.0, 0.0);
    float jacobian = 1.0;
    for (int i = 0; i < 4; ++i) {
        vec4 a = u.wave_a[i];
        vec4 b = u.wave_b[i];
        float t = a.z * dot(a.xy, v_rest) - a.w;
        float wa = a.z * b.x;
        n.x -= a.x * wa * cos(t);
        n.z -= a.y * wa * cos(t);
        n.y -= b.y * wa * sin(t);
        jacobian -= b.y * wa * sin(t);
    }
    vec3 to_eye = u.camera.xyz - v_world;
    float dist = length(to_eye);
    vec3 V = to_eye / max(dist, 1e-4);
    float ripple = (1.0 - smoothstep(15.0, 90.0, dist)) * 0.35;
    for (int i = 0; i < 3; ++i) {
        float k = 6.2831853 / (1.7 * pow(0.53, float(i)));
        float ang = 1.3 + float(i) * 2.1;
        vec2 d = vec2(cos(ang), sin(ang));
        float t = k * dot(d, v_rest) - sqrt(9.81 * k) * u.camera.w;
        float slope = ripple * 0.08 * cos(t);
        n.x -= d.x * slope;
        n.z -= d.y * slope;
    }
    vec3 N = normalize(n);
    bool below = dot(V, vec3(0.0, 1.0, 0.0)) < 0.0 && u.camera.y < v_world.y;
    if (below) N = -N;
    float NdotV = max(dot(N, V), 1e-3);

    // Refraction: distort the screen lookup, unless that would pick up something in front of the water.
    float clarity = max(u.shallow.w, 0.05);
    vec3 behind = world_at(suv, scene_depth);
    float thickness = scene_depth >= 1.0 ? 1e4 : length(behind - v_world);
    vec2 ruv = suv + N.xz * u.params.x * clamp(thickness, 0.0, 2.0) / (1.0 + dist * 0.05);
    float rdepth = textureLod(u_depth, ruv, 0.0).r;
    if (rdepth < gl_FragCoord.z || any(lessThan(ruv, vec2(0.0))) || any(greaterThan(ruv, vec2(1.0)))) {
        ruv = suv;
        rdepth = scene_depth;
    }
    vec3 rbehind = world_at(ruv, rdepth);
    float rthick = rdepth >= 1.0 ? 1e4 : length(rbehind - v_world);
    vec3 under = textureLod(u_scene, ruv, 0.0).rgb;

    vec3 L = normalize(u.sun_dir.xyz);
    float sun_up = clamp(L.y * 4.0, 0.0, 1.0);
    vec3 ambient = sky_radiance(vec3(0.0, 1.0, 0.0), 1.0);
    vec3 body = u.deep.rgb * (ambient + u.sun_color.rgb * max(L.y, 0.0) * 0.25);
    float transmit = exp(-rthick / clarity);
    vec3 tint = mix(vec3(1.0), u.shallow.rgb, clamp(rthick / clarity, 0.0, 1.0));
    vec3 refracted = mix(body, under * tint, transmit);

    // Fresnel (water F0 = 0.02); from below, total internal reflection beyond ~48.6 degrees.
    float fresnel = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);
    vec3 reflected;
    if (below) {
        fresnel = smoothstep(0.62, 0.7, 1.0 - NdotV);
        reflected = body;
    } else {
        vec3 R = reflect(-V, N);
        R.y = abs(R.y);
        reflected = sky_radiance(R, u.params.y);
    }
    vec3 color = mix(refracted, reflected, fresnel);

    if (!below) {
        vec3 H = normalize(L + V);
        float NdotL = max(dot(N, L), 0.0);
        float NdotH = max(dot(N, H), 0.0);
        float alpha = max(u.params.y * u.params.y, 2e-3);
        float a2 = alpha * alpha;
        float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
        float D = a2 / (3.14159265 * dd * dd);
        float F = 0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        color += min(u.sun_color.rgb * (D * F * NdotL / (4.0 * NdotV)), vec3(64.0)) * sun_up;
    }

    // Foam: thin water over geometry (shores, around rocks) and compressed crests.
    float foam_amount = u.deep.w;
    if (foam_amount > 0.0) {
        float shore_depth = rdepth >= 1.0 ? 1e4 : max(v_world.y - behind.y, 0.0);
        float shore = 1.0 - smoothstep(0.0, 0.25 + 0.9 * foam_amount, shore_depth);
        float crest = smoothstep(0.75, 0.25, jacobian);
        float pattern = value_noise(v_rest * 1.9 + vec2(u.camera.w * 0.13, u.camera.w * 0.07)) * 0.6 +
                        value_noise(v_rest * 5.3 - vec2(u.camera.w * 0.21, 0.0)) * 0.4;
        float mask = clamp(max(shore, crest) * foam_amount * 1.6 - (1.0 - pattern), 0.0, 1.0);
        vec3 foam_light = ambient + u.sun_color.rgb * max(dot(N, L), 0.0);
        color = mix(color, vec3(0.92) * foam_light, mask * (below ? 0.0 : 1.0));
    }

    int fog_mode = int(u.fog_color.w + 0.5);
    if (fog_mode > 0 && !below) {
        float f;
        if (fog_mode == 1) f = clamp((dist - u.fog_params.x) / max(u.fog_params.y - u.fog_params.x, 1e-3), 0.0, 1.0);
        else {
            float k = max(dist - u.fog_params.x, 0.0) * u.fog_params.z;
            f = fog_mode == 2 ? 1.0 - exp(-k) : 1.0 - exp(-k * k);
        }
        color = mix(color, u.fog_color.rgb, f * u.fog_params.w);
    }
    // Soft intersection with geometry instead of a hard line.
    float edge = clamp(thickness / 0.08, 0.0, 1.0);
    if (any(isnan(color))) color = body;
    out_color = vec4(max(color, vec3(0.0)), edge);
#ifdef VICTUS_REACTIVE
    // Partly reactive (FSR2 guidance for animated surfaces): moving waves, reflections and refraction
    // have no motion vectors, but the surface itself is smooth, so half the history is still kept.
    out_reactive = vec4(0.5, 0.5, 0.5, edge);
#endif
}
