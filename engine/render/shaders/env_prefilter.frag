#version 450
// SPDX-License-Identifier: Apache-2.0
// Environment bake, step 2: GGX-prefiltered specular radiance for one face of one mip of the
// environment cubemap (split-sum approximation, Karis 2013, with N = V = R). Samples are drawn
// by GGX importance sampling over a Hammersley sequence and read from the mip whose texel
// solid angle matches each sample's (filtered importance sampling, Krivanek & Colbert 2008),
// so few samples converge without fireflies -- cheap enough to bake at load time on phones.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform samplerCube u_source;
layout(set = 3, binding = 0) uniform Prefilter {
    vec4 params; // x: face, y: roughness, z: source mip-0 size, w: sample count
    vec4 params2; // x: source mip count
} p;
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
float radical_inverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
void main() {
    vec3 N = face_direction(int(p.params.x + 0.5), v_uv);
    float roughness = p.params.y;
    if (roughness <= 0.0) {
        out_color = vec4(textureLod(u_source, N, 0.0).rgb, 1.0);
        return;
    }
    float a = roughness * roughness;
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    uint count = uint(p.params.w);
    float size = p.params.z;
    float texel_solid_angle = 4.0 * PI / (6.0 * size * size);
    float max_lod = p.params2.x - 1.0;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < count; ++i) {
        vec2 xi = vec2(float(i) / float(count), radical_inverse(i));
        float phi = 2.0 * PI * xi.x;
        float cos_t = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
        float sin_t = sqrt(1.0 - cos_t * cos_t);
        vec3 H = T * (cos(phi) * sin_t) + B * (sin(phi) * sin_t) + N * cos_t;
        vec3 L = 2.0 * dot(N, H) * H - N;
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        float NdotH = max(cos_t, 0.0);
        float d = NdotH * NdotH * (a * a - 1.0) + 1.0;
        float D = a * a / (PI * d * d);
        float pdf = D * 0.25; // D * NdotH / (4 * VdotH) with V = N
        float sample_solid_angle = 1.0 / (float(count) * pdf + 1e-6);
        float lod = clamp(0.5 * log2(sample_solid_angle / texel_solid_angle) + 1.0, 0.0, max_lod);
        sum += textureLod(u_source, L, lod).rgb * NdotL;
        weight += NdotL;
    }
    out_color = vec4(sum / max(weight, 1e-4), 1.0);
}
