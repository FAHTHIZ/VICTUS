#version 450
// SPDX-License-Identifier: Apache-2.0
// Overdraw debug view for particle billboards and ribbons (ADR-0212): like overdraw.frag, every
// rasterised fragment adds one step to the red channel (all layers), and also to the green channel
// (particle layers), so read_overdraw() can report how much of the fill comes from effects. Whole
// quads count, including their transparent corners: the GPU shades those fragments too.
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(1.0 / 255.0, 1.0 / 255.0, 0.0, 1.0); }
