// SPDX-License-Identifier: Apache-2.0
// Procedural meshes (scene::ProceduralMeshComponent) are uploaded by the GPU asset cache, drawn
// through their MeshRenderer with vertex colours, and dropped once their entity stops using them.
#include "gpu_fixture.hpp"
#include "victus/render/mesh.hpp"
#include "victus/runtime/scene_rendering.hpp"

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
// Two quads on the ground, red for x < 0 and green for x > 0 (counter-clockwise from above).
std::shared_ptr<const ProceduralMesh> halves(std::uint32_t left, std::uint32_t right) {
    ProceduralMesh m;
    const auto quad = [&](float x0, float x1, std::uint32_t c) {
        const auto base = static_cast<std::uint32_t>(m.positions.size());
        m.positions.insert(m.positions.end(), {{x0, 0, 10}, {x1, 0, 10}, {x1, 0, -10}, {x0, 0, -10}});
        m.colors.insert(m.colors.end(), {c, c, c, c});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    quad(-10, 0, left);
    quad(0, 10, right);
    auto p = publish_mesh(std::move(m));
    REQUIRE(p.ok());
    return *p;
}

struct Frame {
    std::vector<std::uint8_t> px;
    runtime::ExtractStats stats;
};

Frame draw(Scene& s, runtime::GpuAssetCache& cache) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    constexpr std::uint32_t W = 96, H = 64;
    auto target = test::make_target(W, H);
    s.update_transforms();
    Frame f;
    f.stats = runtime::extract_scene(s, cache, **sr, {});
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("procedural_mesh", W, H, *px);
    f.px = std::move(*px);
    dev.destroy(target);
    return f;
}
} // namespace

TEST_CASE("gpu: procedural meshes draw with vertex colours and are collected when replaced") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene s;
    const Entity cam = s.create("Camera");
    s.world().get<Transform>(cam)->position = {0, 8, 0};
    s.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
    s.world().add<CameraComponent>(cam);
    const Entity ground = s.create("Ground");
    auto& mr = s.world().add<MeshRendererComponent>(ground);
    mr.primitive = "cube"; // replaced by the procedural mesh
    mr.roughness = 1.0f;
    s.world().add<ProceduralMeshComponent>(ground).mesh =
        halves(render::pack_vertex_color_srgb8(220, 20, 20), render::pack_vertex_color_srgb8(20, 220, 20));
    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    Frame a = draw(s, cache);
    CHECK(a.stats.meshes == 1);
    CHECK(a.stats.missing_assets == 0);
    const auto l = pixel(a.px, 96, 20, 32), r = pixel(a.px, 96, 76, 32);
    MESSAGE("left " << channel(l, 0) << "," << channel(l, 1) << " right " << channel(r, 0) << "," << channel(r, 1));
    CHECK(channel(l, 0) > channel(l, 1) + 60);
    CHECK(channel(r, 1) > channel(r, 0) + 60);
    CHECK(cache.procedural_count() == 1);

    // Swap the colours with a new mesh every frame: old GPU copies are released after a few frames.
    for (int i = 0; i < 12; ++i) {
        s.world().get<ProceduralMeshComponent>(ground)->mesh =
            halves(render::pack_vertex_color_srgb8(20, 220, 20), render::pack_vertex_color_srgb8(220, 20, 20));
        a = draw(s, cache);
    }
    CHECK(cache.procedural_count() <= 9);
    const auto l2 = pixel(a.px, 96, 20, 32);
    CHECK(channel(l2, 1) > channel(l2, 0) + 60);

    // Hidden renderers draw nothing; removing the component returns to the primitive.
    s.world().get<MeshRendererComponent>(ground)->visible = false; // components may move when others are added
    CHECK(draw(s, cache).stats.meshes == 0);
    s.world().get<MeshRendererComponent>(ground)->visible = true;
    s.world().remove<ProceduralMeshComponent>(ground);
    for (int i = 0; i < 10; ++i) a = draw(s, cache);
    CHECK(cache.procedural_count() == 0);
    CHECK(a.stats.meshes == 1); // the cube
    // Empty geometry is skipped without errors.
    ProceduralMesh empty;
    s.world().add<ProceduralMeshComponent>(ground).mesh = *publish_mesh(std::move(empty));
    CHECK(draw(s, cache).stats.meshes == 0);
    cache.clear();
}
