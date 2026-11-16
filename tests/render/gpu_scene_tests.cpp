// SPDX-License-Identifier: Apache-2.0
// End-to-end vertical slice: glTF source → asset database (meta + cook) → model instantiated
// into a scene → scene saved to disk and reloaded → extracted and rendered on the GPU.
#include "../assets/gltf_writer.hpp"
#include "gpu_fixture.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <filesystem>

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
math::Vec2 to_screen(const render::Camera3D& cam, float aspect, math::Vec3 p, float w, float h) {
    const math::Vec4 clip = cam.projection(aspect) * cam.view * math::Vec4(p, 1.0f);
    const float x = clip.x / clip.w, y = clip.y / clip.w;
    return {(x * 0.5f + 0.5f) * w, (1.0f - (y * 0.5f + 0.5f)) * h};
}
} // namespace

TEST_CASE("gpu: glTF asset → scene file → rendered frame") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();

    const fs::Path project = std::filesystem::temp_directory_path() / ("victus_e2e_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(project / "assets" / "models");
    test::GltfFixture fx;
    // Solid white albedo so the quad colour is the material factor (0.8, 0.2, 0.1).
    auto png = render::encode_png(render::make_checker(8, 2, 0xffffffffu, 0xffffffffu));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(project / "assets" / "models" / "albedo.png", *png).ok());
    fx.add_texture("albedo.png");
    REQUIRE(fs::write_atomic(project / "assets" / "models" / "quad.glb", fx.glb()).ok());

    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    const assets::AssetRecord* rec = (*db)->find_path("models/quad.glb");
    REQUIRE(rec);
    const Uuid model_id = rec->uuid;

    // Author the scene through the public API and save it.
    {
        Scene authored;
        authored.name = "E2E";
        auto model = (*db)->load_model(model_id);
        REQUIRE(model.ok());
        runtime::instantiate_model(authored, model_id, *model, "Quad Model");
        const Entity cube = authored.create("Blue Cube");
        authored.world().get<Transform>(cube)->position = {-2, 0.5f, 0};
        auto& mr = authored.world().add<MeshRendererComponent>(cube);
        mr.primitive = "cube";
        mr.base_color = {0.05f, 0.1f, 0.9f, 1};
        const Entity sun = authored.create("Sun");
        authored.world().get<Transform>(sun)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-70));
        authored.world().add<LightComponent>(sun).intensity = 3.0f;
        const Entity cam = authored.create("Camera");
        authored.world().get<Transform>(cam)->position = {0, 9, 3};
        authored.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-72));
        authored.world().add<CameraComponent>(cam);
        REQUIRE(authored.save_file(project / "assets" / "main.vscene").ok());
    }

    Scene scene;
    REQUIRE(scene.load_file(project / "assets" / "main.vscene").ok());
    CHECK(scene.size() == 6); // model root + Root + Child + cube + sun + camera
    scene.update_transforms();

    runtime::GpuAssetCache cache(ctx, db->get());
    auto sr = render::SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    const std::uint32_t W = 192, H = 128;
    auto target = test::make_target(W, H);
    runtime::SceneViewSettings view;
    const auto stats = runtime::extract_scene(scene, cache, **sr, view);
    CHECK(stats.camera_found);
    CHECK(stats.meshes == 3);
    CHECK(stats.missing_assets == 0);
    CHECK(stats.lights == 1);
    auto cmd = dev.begin_commands();
    auto st = (*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("scene_e2e", W, H, *px);

    // Reconstruct the camera exactly as extraction did to locate objects on screen.
    const Entity cam_e = scene.find_by_name("Camera");
    math::Vec3 cp, cs;
    math::Quat cr;
    math::decompose(scene.world().get<WorldTransform>(cam_e)->matrix, cp, cr, cs);
    const auto camera = render::Camera3D::from_transform(cp, cr);
    const float aspect = static_cast<float>(W) / static_cast<float>(H);

    // Child quad: world centre (2, 1, 0), 2x2 → reddish/orange (material factor).
    const math::Vec2 q = to_screen(camera, aspect, {2, 1, 0}, static_cast<float>(W), static_cast<float>(H));
    const auto qp = pixel(*px, W, static_cast<std::uint32_t>(q.x), static_cast<std::uint32_t>(q.y));
    CHECK(channel(qp, 0) > channel(qp, 1) + 40);
    CHECK(channel(qp, 0) > channel(qp, 2) + 40);
    // Cube top at (-2, 1, 0) → blue.
    const math::Vec2 c = to_screen(camera, aspect, {-2, 1, 0}, static_cast<float>(W), static_cast<float>(H));
    const auto cpx = pixel(*px, W, static_cast<std::uint32_t>(c.x), static_cast<std::uint32_t>(c.y));
    CHECK(channel(cpx, 2) > channel(cpx, 0) + 40);

    cache.clear();
    dev.destroy(target);
    std::error_code ec;
    std::filesystem::remove_all(project, ec);
}
