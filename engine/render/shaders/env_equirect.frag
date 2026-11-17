#version 450
// SPDX-License-Identifier: Apache-2.0
// Environment bake, step 1: resamples an equirectangular (latitude-longitude) HDR image into
// one face of a cubemap. Equirect convention: u = 0.5 looks down -Z, v = 0 is straight up.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_equirect;
layout(set = 3, binding = 0) uniform Face {
    vec4 face; // x: cube face index (+X, -X, +Y, -Y, +Z, -Z)
} f;
layout(location = 0) out vec4 out_color;
const float PI = 3.14159265359;
vec3 face_direction(int face, vec2 uv) {
    float sc = uv.x * 2.0 - 1.0, tc = uv.y * 2.0 - 1.0;
    if (face == 0) return normalize(vec3(1.0, -tc, -sc));
    if (face == 1) return normalize(vec3(-1.0, -tc, sc));
    if (face == 2) return normalize(vec3(sc, 1.0, tc));
    if (face == 3) return normalize(vec3(sc, -1.0, -tc));
    if (face == 4) return normalize(vec3(sc, -tc, 1.0));
    return normalize(vec3(-sc, -tc, -1.0));
}
void main() {
    vec3 d = face_direction(int(f.face.x + 0.5), v_uv);
    vec2 uv = vec2(atan(d.x, -d.z) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
    // Explicit LOD 0: the derivative across the atan seam would otherwise pick a blurry mip.
    out_color = vec4(textureLod(u_equirect, uv, 0.0).rgb, 1.0);
}
