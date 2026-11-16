#version 450
// SPDX-License-Identifier: Apache-2.0
// Auto exposure step 2: centre-weighted average of the 64x64 log-luminance image, clamped to
// the allowed EV range and adapted exponentially towards the previous frame's value.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_lum;
layout(set = 2, binding = 1) uniform sampler2D u_previous;
layout(set = 3, binding = 0) uniform Exposure {
    vec4 params; // x: blend up, y: blend down (0..1 per frame), z: min log2 lum, w: max log2 lum
    vec4 flags;  // x: reset (use target directly)
} u;
layout(location = 0) out vec4 out_value;
void main() {
    float sum = 0.0, wsum = 0.0;
    // 32x32 bilinear taps cover the 64x64 image (each tap averages 2x2 texels).
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            vec2 uv = (vec2(x, y) + 0.5) / 32.0;
            vec2 c = uv - 0.5;
            float w = 1.0 - 0.75 * clamp(dot(c, c) * 2.0, 0.0, 1.0);
            sum += textureLod(u_lum, uv, 0.0).r * w;
            wsum += w;
        }
    float target = clamp(sum / wsum, u.params.z, u.params.w);
    float previous = textureLod(u_previous, vec2(0.5), 0.0).r;
    float value = target;
    if (u.flags.x < 0.5) value = mix(previous, target, target > previous ? u.params.x : u.params.y);
    out_value = vec4(value, 0.0, 0.0, 1.0);
}
