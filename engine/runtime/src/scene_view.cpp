// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/scene_view.hpp"

#include "victus/core/profiler.hpp"

namespace victus::runtime {

using namespace scene;

namespace {
const CameraComponent* primary_camera(Scene& scene, Entity* out_entity) {
    const CameraComponent* found = nullptr;
    scene.world().each<CameraComponent>([&](Entity e, const CameraComponent& c) {
        if (found || !c.primary) return;
        found = &c;
        if (out_entity) *out_entity = e;
    });
    return found;
}
} // namespace

ViewMode scene_view_mode(Scene& scene) {
    const CameraComponent* c = primary_camera(scene, nullptr);
    return c && c->projection == CameraComponent::Projection::Orthographic ? ViewMode::TwoD : ViewMode::ThreeD;
}

std::optional<render::Camera2D> scene_camera_2d(Scene& scene, float viewport_height) {
    Entity e;
    const CameraComponent* c = primary_camera(scene, &e);
    if (!c || c->projection != CameraComponent::Projection::Orthographic) return std::nullopt;
    const WorldTransform* wt = scene.world().get<WorldTransform>(e);
    math::Vec3 pos, scale;
    math::Quat rot;
    if (wt) math::decompose(wt->matrix, pos, rot, scale);
    render::Camera2D cam;
    cam.position = {pos.x, pos.y};
    cam.rotation = math::to_euler(rot).z;
    cam.pixels_per_unit = viewport_height / std::max(c->ortho_height, 0.01f);
    return cam;
}

Result<std::unique_ptr<SceneView>> SceneView::create(render::GpuContext& ctx, rhi::Format target_format) {
    std::unique_ptr<SceneView> v(new SceneView());
    v->format_ = target_format;
    auto r = render::SceneRenderer::create(ctx);
    if (!r) return std::move(r).error();
    v->renderer_ = std::move(r).value();
    auto s = render::SpriteBatch::create(ctx, target_format);
    if (!s) return std::move(s).error();
    v->sprites_ = std::move(s).value();
    return v;
}

Result<SceneViewStats> SceneView::render(rhi::CommandBuffer& cmd, Scene& scene, GpuAssetCache& assets,
                                         rhi::TextureHandle target, std::uint32_t width, std::uint32_t height,
                                         const SceneViewOptions& options) {
    VICTUS_PROFILE_SCOPE("runtime.scene_view");
    SceneViewStats stats;
    stats.mode = options.mode.value_or(scene_view_mode(scene));
    if (width == 0 || height == 0) return stats;
    if (stats.mode == ViewMode::TwoD) {
        render::Camera2D cam;
        if (options.camera_2d) cam = *options.camera_2d;
        else if (auto c = scene_camera_2d(scene, static_cast<float>(height))) cam = *c;
        else cam.pixels_per_unit = static_cast<float>(height) / 10.0f;
        sprites_->begin(cam.view_projection(static_cast<float>(width), static_cast<float>(height)));
        stats.sprites = extract_sprites(scene, assets, *sprites_);
        if (options.overlay_2d) options.overlay_2d(*sprites_);
        VICTUS_TRY_STATUS(sprites_->prepare(cmd));
        rhi::RenderPassDesc rp;
        rhi::ColorAttachment color;
        color.texture = target;
        color.load = rhi::LoadOp::Clear;
        color.clear = options.clear_2d;
        rp.colors.push_back(color);
        auto pass = cmd.begin_render_pass(rp);
        sprites_->render(pass, cmd);
        pass.end();
        return stats;
    }
    stats.extract = extract_scene(scene, assets, *renderer_, options.settings, options.camera_3d);
    if (options.overlay_3d) options.overlay_3d(*renderer_);
    VICTUS_TRY_STATUS(renderer_->render(cmd, target, width, height, format_));
    stats.scene = renderer_->stats();
    return stats;
}

} // namespace victus::runtime
