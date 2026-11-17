#version 450
// SPDX-License-Identifier: Apache-2.0
// Object motion vectors (ADR-0054). Drawn after the main pass for instances that moved since the
// previous frame, depth-tested (LessEqual, no writes) against the scene depth: the position math
// matches mesh.vert / depth.vert bit for bit (invariant). The fragment stage writes how far the
// surface moved by itself, i.e. the previous-camera projection of its current position minus that
// of its previous position; the TAA resolve adds this to its camera-only reprojection.
layout(location = 0) in vec3 in_pos;
layout(location = 4) in vec4 in_model0;
layout(location = 5) in vec4 in_model1;
layout(location = 6) in vec4 in_model2;
layout(location = 7) in vec4 in_model3;
layout(location = 8) in vec4 in_prev_row0; // previous model matrix, affine rows
layout(location = 9) in vec4 in_prev_row1;
layout(location = 10) in vec4 in_prev_row2;
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;      // jittered, as the main pass
    mat4 prev_view_proj; // previous frame, unjittered
} frame;
layout(location = 0) out vec4 v_current; // previous-camera clip position of the current point
layout(location = 1) out vec4 v_previous; // ... of the same point last frame
invariant gl_Position;
void main() {
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    vec4 world = model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
    vec4 p = vec4(in_pos, 1.0);
    vec3 prev_world = vec3(dot(in_prev_row0, p), dot(in_prev_row1, p), dot(in_prev_row2, p));
    v_current = frame.prev_view_proj * world;
    v_previous = frame.prev_view_proj * vec4(prev_world, 1.0);
}
