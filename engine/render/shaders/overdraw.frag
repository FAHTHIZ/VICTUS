#version 450
// SPDX-License-Identifier: Apache-2.0
// Overdraw debug view (RenderSettings::debug_view = Overdraw): every rasterised fragment adds one
// step (1/255) to an RGBA8 counter with additive blending, so the red channel holds the number of
// layers drawn into each pixel (saturating at 255).
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(1.0 / 255.0, 0.0, 0.0, 1.0); }
