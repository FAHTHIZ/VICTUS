#version 450
// SPDX-License-Identifier: Apache-2.0
// Sky background per view ray: the scene's environment cubemap (optionally blurred by sampling a
// prefiltered mip) or the procedural zenith/horizon/ground gradient plus sun disc.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform samplerCube u_env;
layout(set = 3, binding = 0) uniform Sky {
    mat4 inv_view_proj;
    vec4 zenith;
    vec4 horizon;
    vec4 ground;
    vec4 sun_dir;   // direction towards the sun, w: compact HDR frame + 1 (0 off)
    vec4 sun_color; // rgb * intensity, w: disc size (cos)
    vec4 env;       // x: environment map present (2: also draw the sun disc), y: mip (blur), z: intensity, w: rotation (radians about +Y)
} sky;

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
layout(location = 0) out vec4 out_color;
void main() {
    vec2 ndc = vec2(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0);
    vec4 far_p = sky.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec4 near_p = sky.inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec3 dir = normalize(far_p.xyz / far_p.w - near_p.xyz / near_p.w);
    if (sky.env.x > 0.5) {
        float c = cos(sky.env.w), s = sin(sky.env.w);
        vec3 d = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
        vec3 e = textureLod(u_env, d, sky.env.y).rgb * sky.env.z;
        // Physical skies are baked without the sun disc (too small for the cube): add it here.
        if (sky.env.x > 1.5) e += sky.sun_color.rgb * smoothstep(sky.sun_color.w, 1.0, dot(dir, normalize(sky.sun_dir.xyz)));
        out_color = vec4(e, 1.0);
        if (sky.sun_dir.w > 0.0) out_color.rgb = hdr_quantize(out_color.rgb, sky.sun_dir.w);
        return;
    }
    float h = dir.y;
    vec3 c = h > 0.0 ? mix(sky.horizon.rgb, sky.zenith.rgb, pow(h, 0.5)) : mix(sky.horizon.rgb, sky.ground.rgb, pow(-h, 0.4));
    float s = dot(dir, normalize(sky.sun_dir.xyz));
    c += sky.sun_color.rgb * smoothstep(sky.sun_color.w, 1.0, s) + sky.sun_color.rgb * 0.08 * pow(max(s, 0.0), 64.0);
    out_color = vec4(c, 1.0);
    if (sky.sun_dir.w > 0.0) out_color.rgb = hdr_quantize(out_color.rgb, sky.sun_dir.w);
}
