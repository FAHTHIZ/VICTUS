#version 450
// SPDX-License-Identifier: Apache-2.0
// Auto exposure step 1: log2 luminance of the HDR image at a small fixed resolution
// (each output texel averages a 2x2 bilinear footprint of 4 taps).
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_hdr;
layout(set = 3, binding = 0) uniform Lum { vec4 params; } u; // xy: 1 / output size
layout(location = 0) out vec4 out_lum;
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
void main() {
    vec2 q = u.params.xy * 0.25;
    float l = 0.0;
    l += log2(max(luma(textureLod(u_hdr, v_uv + vec2(-q.x, -q.y), 0.0).rgb), 1e-5));
    l += log2(max(luma(textureLod(u_hdr, v_uv + vec2(q.x, -q.y), 0.0).rgb), 1e-5));
    l += log2(max(luma(textureLod(u_hdr, v_uv + vec2(-q.x, q.y), 0.0).rgb), 1e-5));
    l += log2(max(luma(textureLod(u_hdr, v_uv + vec2(q.x, q.y), 0.0).rgb), 1e-5));
    out_lum = vec4(l * 0.25, 0.0, 0.0, 1.0);
}
