#version 450
// SPDX-License-Identifier: Apache-2.0
// Volumetric fog, pass 3 of 3 (ADR-0098): composites the integrated froxel grid over the scene.
// Output (in-scattered light, 1 - transmittance) with premultiplied-alpha blending, so
// result = scene * transmittance + in-scattered light. The grid is fetched trilinearly at the
// pixel's view depth (sky pixels: the far end of the grid).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_depth;
// SPIR-V: after the one sampler in set 2; MSL: after the one uniform buffer. Both are binding 1.
layout(std430, set = 2, binding = 1) readonly buffer Integrated { vec4 integrated[]; };
layout(set = 3, binding = 0) uniform Params {
    vec4 proj;   // view depth = proj.x / (depth + proj.y)
    vec4 screen; // xy: 1 / render size
    vec4 grid;   // xyz froxels, w: grid far distance
} u;

vec4 column(uvec2 a, uvec2 b, vec2 w, uint z) {
    uvec3 g = uvec3(u.grid.xyz);
    uint base = z * g.y * g.x;
    vec4 c0 = mix(integrated[base + a.y * g.x + a.x], integrated[base + a.y * g.x + b.x], w.x);
    vec4 c1 = mix(integrated[base + b.y * g.x + a.x], integrated[base + b.y * g.x + b.x], w.x);
    return mix(c0, c1, w.y);
}

void main() {
    vec2 uv = gl_FragCoord.xy * u.screen.xy;
    float depth = u.proj.x / (textureLod(u_depth, uv, 0.0).r + u.proj.y);
    vec3 g = u.grid.xyz;
    vec2 x = clamp(uv * g.xy - 0.5, vec2(0.0), g.xy - 1.0);
    uvec2 a = uvec2(floor(x));
    uvec2 b = min(a + 1u, uvec2(g.xy) - 1u);
    vec2 w = x - vec2(a);
    // integrated[z] holds the result at the far edge of slice z, i.e. at s = z + 1 where s = sqrt(depth / far) * slices.
    float s = sqrt(clamp(depth / u.grid.w, 0.0, 1.0)) * g.z;
    vec4 v;
    if (s < 1.0) {
        v = mix(vec4(0.0, 0.0, 0.0, 1.0), column(a, b, w, 0u), s);
    } else {
        float f = s - 1.0;
        uint z0 = min(uint(floor(f)), uint(g.z) - 1u);
        uint z1 = min(z0 + 1u, uint(g.z) - 1u);
        v = mix(column(a, b, w, z0), column(a, b, w, z1), f - float(z0));
    }
    out_color = vec4(v.rgb, 1.0 - clamp(v.a, 0.0, 1.0));
}
