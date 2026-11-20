// SPDX-License-Identifier: Apache-2.0
// One call to draw a scene into a colour target, shared by the editor viewport and the game
// player. Scenes whose primary camera is orthographic render as 2D (sprites, layer-sorted);
// all others render through the 3D SceneRenderer.
#pragma once
#include "victus/runtime/game_ui.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace victus::runtime {

enum class ViewMode : std::uint8_t { ThreeD, TwoD };

// 2D when the first primary camera is orthographic.
[[nodiscard]] ViewMode scene_view_mode(scene::Scene& scene);
// Camera2D equivalent of the scene's primary orthographic camera for a viewport height.
[[nodiscard]] std::optional<render::Camera2D> scene_camera_2d(scene::Scene& scene, float viewport_height);

struct SceneViewOptions {
    SceneViewSettings settings;
    std::optional<ViewMode> mode;                 // default: scene_view_mode()
    const render::Camera3D* camera_3d{nullptr};   // override (editor camera)
    const render::Camera2D* camera_2d{nullptr};   // override (editor camera)
    std::function<void(render::SceneRenderer&)> overlay_3d; // extra lines/meshes after extraction
    std::function<void(render::SpriteBatch&)> overlay_2d;   // extra sprites/lines after extraction
    math::Color clear_2d{0.08f, 0.085f, 0.1f, 1};
    bool draw_ui{true};                           // in-game UI canvases over the frame
};

struct SceneViewStats {
    ViewMode mode{ViewMode::ThreeD};
    ExtractStats extract;
    std::uint32_t sprites{0};
    std::uint32_t lights_2d{0}; // 2D lights drawn (0 also when the scene has no 2D lighting)
    bool lighting_2d{false};    // the 2D frame was lit
    render::SceneStats scene;
    GameUiStats ui;
};

class SceneView {
public:
    // `options` are fixed for the view's lifetime (compact_hdr changes the pipelines' formats).
    [[nodiscard]] static Result<std::unique_ptr<SceneView>> create(render::GpuContext& ctx, rhi::Format target_format,
                                                                   const render::SceneRendererOptions& options = {});
    // Records the frame for `scene` (WorldTransforms must be current) into `target`.
    [[nodiscard]] Result<SceneViewStats> render(rhi::CommandBuffer& cmd, scene::Scene& scene, GpuAssetCache& assets,
                                                rhi::TextureHandle target, std::uint32_t width, std::uint32_t height,
                                                const SceneViewOptions& options = {});
    [[nodiscard]] render::SceneRenderer& renderer() noexcept { return *renderer_; }
    [[nodiscard]] render::SpriteBatch& sprites() noexcept { return *sprites_; }

private:
    SceneView() = default;
    rhi::Format format_{rhi::Format::RGBA8_UNORM};
    std::unique_ptr<render::SceneRenderer> renderer_;
    std::unique_ptr<render::SpriteBatch> sprites_;
    std::unique_ptr<GameUiRenderer> ui_; // created on first scene with a UI canvas
    [[nodiscard]] Status render_ui(rhi::CommandBuffer& cmd, scene::Scene& scene, GpuAssetCache& assets, rhi::TextureHandle target,
                                   std::uint32_t width, std::uint32_t height, const SceneViewOptions& options, SceneViewStats& stats);
    render::GpuContext* gpu_{nullptr};
};

} // namespace victus::runtime
