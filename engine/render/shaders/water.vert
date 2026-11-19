#version 450
// SPDX-License-Identifier: Apache-2.0
// Gerstner water surface (ADR-0095). A cells x cells grid comes from gl_VertexIndex (no vertex
// buffer) and is displaced by the same four waves gameplay::sample_waves evaluates on the CPU,
// so buoyancy matches what is drawn. Waves shorter than ~4 grid cells fade out of the geometry
// (they would alias) but stay in the per-pixel normal. Border vertices keep their horizontal
// position so the surface always covers its volume.
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 center;    // xyz rest surface centre, w cells per side
    vec4 extent;    // xy half extent (m), z cell size (m)
    vec4 wave_a[4]; // xy direction, z k, w phase
    vec4 wave_b[4]; // x amplitude, y q (horizontal displacement factor)
} frame;
layout(location = 0) out vec3 v_world;
layout(location = 1) out vec2 v_rest;
void main() {
    int cells = int(frame.center.w + 0.5);
    int quad = gl_VertexIndex / 6;
    int corner = gl_VertexIndex - quad * 6;
    const ivec2 offs[6] = ivec2[](ivec2(0, 0), ivec2(0, 1), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(1, 0));
    ivec2 cell = ivec2(quad % cells, quad / cells) + offs[corner];
    vec2 uv = vec2(cell) / float(cells);
    vec2 xz = frame.center.xz + (uv * 2.0 - 1.0) * frame.extent.xy;
    float inner = (cell.x == 0 || cell.y == 0 || cell.x == cells || cell.y == cells) ? 0.0 : 1.0;
    vec3 p = vec3(xz.x, frame.center.y, xz.y);
    for (int i = 0; i < 4; ++i) {
        vec4 a = frame.wave_a[i];
        vec4 b = frame.wave_b[i];
        float wavelength = 6.2831853 / max(a.z, 1e-4);
        float fade = smoothstep(2.0 * frame.extent.z, 4.0 * frame.extent.z, wavelength);
        float t = a.z * dot(a.xy, xz) - a.w;
        float amp = b.x * fade;
        float c = cos(t) * b.y * amp * inner;
        p.x += a.x * c;
        p.z += a.y * c;
        p.y += amp * sin(t);
    }
    v_world = p;
    v_rest = xz;
    gl_Position = frame.view_proj * vec4(p, 1.0);
}
