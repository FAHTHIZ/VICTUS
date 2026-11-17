#version 450
// SPDX-License-Identifier: Apache-2.0
// Projected decal box: the 36 corners of the unit cube [-0.5, 0.5]^3 come from gl_VertexIndex
// (no vertex buffer). Triangles wind counter-clockwise seen from outside; the pipeline culls
// front faces so every covered pixel is shaded once, also with the camera inside the box.
layout(set = 1, binding = 0) uniform Frame {
    mat4 mvp;
} frame;
void main() {
    const vec3 corners[8] = vec3[](vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5),
                                   vec3(-0.5, 0.5, -0.5), vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5),
                                   vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
    const int indices[36] = int[](4, 5, 6, 4, 6, 7,  // +Z
                                  1, 0, 3, 1, 3, 2,  // -Z
                                  5, 1, 2, 5, 2, 6,  // +X
                                  0, 4, 7, 0, 7, 3,  // -X
                                  7, 6, 2, 7, 2, 3,  // +Y
                                  0, 1, 5, 0, 5, 4); // -Y
    gl_Position = frame.mvp * vec4(corners[indices[gl_VertexIndex]], 1.0);
}
