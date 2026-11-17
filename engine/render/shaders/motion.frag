#version 450
// SPDX-License-Identifier: Apache-2.0
// Object motion in texture space (previous-camera uv of the current point minus that of its
// previous position); 0 = the surface did not move by itself.
layout(location = 0) in vec4 v_current;
layout(location = 1) in vec4 v_previous;
layout(location = 0) out vec4 out_motion;
void main() {
    vec2 a = v_current.xy / max(v_current.w, 1e-6);
    vec2 b = v_previous.xy / max(v_previous.w, 1e-6);
    out_motion = vec4(0.5 * (a.x - b.x), -0.5 * (a.y - b.y), 0.0, 0.0);
}
