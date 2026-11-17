#version 450
// SPDX-License-Identifier: Apache-2.0
// Projected decals in the forward pipeline (ADR-0064): the pass has no depth attachment. Each
// pixel of the decal box reconstructs the opaque surface behind it from scene depth, moves it
// into decal space and, inside the unit box, samples the decal texture along the local -Z axis.
// Derivatives are taken before any discard (they are undefined in non-uniform control flow).
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Params {
    mat4 inv_view_proj;  // jittered, matches the depth buffer
    mat4 world_to_decal; // world -> unit box
    vec4 color;          // linear rgba (alpha = opacity)
    vec4 uv;             // u0, v0, u1, v1
    vec4 screen;         // xy: 1 / render size, z: blend (0 multiply, 1 alpha, 2 additive), w: angle fade
    vec4 axis;           // xyz: world projection axis (unit), w: depth fade fraction
} u;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 suv = gl_FragCoord.xy * u.screen.xy;
    float depth = textureLod(u_depth, suv, 0.0).r;
    vec2 ndc = vec2(suv.x * 2.0 - 1.0, 1.0 - suv.y * 2.0);
    vec4 wp = u.inv_view_proj * vec4(ndc, depth, 1.0);
    vec3 world = wp.xyz / wp.w;
    vec3 local = (u.world_to_decal * vec4(world, 1.0)).xyz;
    vec2 tuv = vec2(local.x + 0.5, 0.5 - local.y);
    vec2 uv = mix(u.uv.xy, u.uv.zw, tuv);
    vec2 duv_dx = dFdx(uv), duv_dy = dFdy(uv);
    vec3 n = cross(dFdx(world), dFdy(world));
    // Depth discontinuities (decal edges on silhouettes) give huge derivatives: a pixel spanning more
    // than half the decal falls back to the full-resolution level instead of a blurry seam.
    float limit = 0.5 * max(abs(u.uv.z - u.uv.x), abs(u.uv.w - u.uv.y));
    if (max(length(duv_dx), length(duv_dy)) > limit) duv_dx = duv_dy = vec2(0.0);

    if (depth >= 1.0) discard;
    vec3 a = abs(local);
    if (a.x > 0.5 || a.y > 0.5 || a.z > 0.5) discard;
    float weight = 1.0;
    float nl = length(n);
    if (u.screen.w > 0.0 && nl > 0.0) {
        float facing = abs(dot(n / nl, u.axis.xyz));
        weight *= smoothstep(u.screen.w * 0.5, u.screen.w, facing);
    }
    if (u.axis.w > 0.0) weight *= 1.0 - smoothstep(0.5 - u.axis.w * 0.5, 0.5, a.z);
    vec4 c = textureGrad(u_texture, uv, duv_dx, duv_dy) * u.color;
    float alpha = clamp(c.a * weight, 0.0, 1.0);
    if (alpha <= 0.002) discard;
    if (u.screen.z < 0.5) out_color = vec4(mix(vec3(1.0), max(c.rgb, vec3(0.0)), alpha), 0.0); // dst * src
    else out_color = vec4(max(c.rgb, vec3(0.0)), alpha);                                       // alpha / additive
}
