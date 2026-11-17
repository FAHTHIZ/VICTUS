// SPDX-License-Identifier: Apache-2.0
// Scene captures for local reflection probes: six 90-degree views rendered in linear HDR,
// read back and reassembled into an equirect that EnvironmentMap bakes like any HDR sky.
#pragma once
#include "victus/render/environment_map.hpp"
#include "victus/render/scene_renderer.hpp"

#include <functional>

namespace victus::render {

// Fills the renderer for one face: must call renderer.begin(camera, ...) and submit the scene.
using CaptureFill = std::function<void(SceneRenderer& renderer, const Camera3D& camera)>;

// Renders the six faces around `position` (face_size^2 each, 16..1024) and returns a
// (4*face_size) x (2*face_size) linear equirect. The renderer is switched to capture mode
// (linear output, no FXAA/bloom/auto exposure/vignette) for the duration and restored after.
[[nodiscard]] Result<ImageF> capture_equirect(GpuContext& ctx, SceneRenderer& renderer, math::Vec3 position,
                                              std::uint32_t face_size, const CaptureFill& fill,
                                              float z_near = 0.05f, float z_far = 500.0f);

// Reassembles six faces (order +X, -X, +Y, -Y, +Z, -Z; each RGBA32F, rendered with the cameras
// capture_face_camera returns) into an equirect of the given width (height = width / 2).
[[nodiscard]] ImageF faces_to_equirect(const std::array<ImageF, 6>& faces, std::uint32_t width);
[[nodiscard]] Camera3D capture_face_camera(math::Vec3 position, int face, float z_near, float z_far);

} // namespace victus::render
