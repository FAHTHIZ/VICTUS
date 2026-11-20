// SPDX-License-Identifier: Apache-2.0
// GPU occlusion culling (Slice 55, ADR-0051): RHI compute pipelines, storage buffer readback,
// indirect draws, and the renderer's Hi-Z test hiding instances behind a wall without changing
// the image.
#include "gpu_fixture.hpp"
#include "victus/render/builtin_shaders.hpp"
#include "victus/render/scene_renderer.hpp"

#include <array>
#include <cstring>

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

TEST_CASE("gpu compute: built-in pipeline reduces a buffer and reads it back") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& dev = gpu().ctx->device();
    REQUIRE(dev.capabilities().compute);
    auto pl = create_builtin_compute_pipeline(dev, "hiz_reduce.comp");
    REQUIRE(pl.ok());
    CHECK(dev.stats().live_compute_pipelines >= 1);
    CHECK_FALSE(create_builtin_compute_pipeline(dev, "mesh.vert").ok()); // not a compute shader

    // 4x4 source level at offset 0, 2x2 destination at offset 16.
    std::array<float, 20> data{};
    for (int i = 0; i < 16; ++i) data[static_cast<std::size_t>(i)] = static_cast<float>(i) / 16.0f;
    auto buf = dev.create_buffer({"compute_test", sizeof(data), rhi::BufferUsageStorageWrite});
    REQUIRE(buf.ok());
    const std::uint64_t dispatches = dev.stats().dispatches;
    {
        auto cmd = dev.begin_commands();
        cmd.upload_buffer(buf.value(), 0, {reinterpret_cast<const std::uint8_t*>(data.data()), sizeof(data)});
        const rhi::BufferHandle rw[] = {buf.value()};
        rhi::ComputePass cp = cmd.begin_compute_pass(rw);
        REQUIRE(cp.active());
        cp.bind_pipeline(pl.value());
        const std::array<std::int32_t, 8> params{0, 4, 4, 0, 16, 2, 2, 0};
        cmd.push_uniforms(rhi::ShaderStage::Compute, 0, params);
        cp.dispatch(1, 1);
        cp.end();
        cmd.submit_and_wait();
    }
    CHECK(dev.stats().dispatches == dispatches + 1);
    auto out = dev.read_buffer(buf.value(), 16 * 4, 16);
    REQUIRE(out.ok());
    REQUIRE(out.value().size() == 16);
    std::array<float, 4> r{};
    std::memcpy(r.data(), out.value().data(), 16);
    CHECK(r[0] == doctest::Approx(5.0f / 16));
    CHECK(r[1] == doctest::Approx(7.0f / 16));
    CHECK(r[2] == doctest::Approx(13.0f / 16));
    CHECK(r[3] == doctest::Approx(15.0f / 16));
    auto whole = dev.read_buffer(buf.value());
    REQUIRE(whole.ok());
    CHECK(whole.value().size() == sizeof(data));
    CHECK_FALSE(dev.read_buffer(buf.value(), 64, 32).ok()); // out of range
    CHECK_FALSE(dev.read_buffer(rhi::BufferHandle{}).ok());
    dev.destroy(buf.value());
    dev.destroy(pl.value());
}

namespace {

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
    std::uint32_t survivors{0};
    std::uint64_t indirect_draws{0};
};

struct WallScene {
    GpuMesh cube;
    Material wall, box, front;
    float wall_height{8};
    void submit(SceneRenderer& r, bool hidden_grid = true) const {
        r.submit(cube, wall, math::Mat4::translation({0, 0, 3}) * math::Mat4::scale({12, wall_height, 0.4f}));
        if (hidden_grid)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    r.submit(cube, box, math::Mat4::translation({-3.0f + 2.0f * x, -1.5f + y, -3.0f}));
        r.submit(cube, front, math::Mat4::translation({-1.2f, 0, 5.5f}) * math::Mat4::scale({0.5f, 0.5f, 0.5f}));
        r.submit(cube, front, math::Mat4::translation({1.2f, 0.3f, 5.5f}) * math::Mat4::scale({0.5f, 0.5f, 0.5f}));
    }
};

Shot shoot(SceneRenderer& r, RenderSettings s, const WallScene& scene, int frames = 1, bool hidden_grid = true) {
    auto& dev = gpu().ctx->device();
    constexpr std::uint32_t W = 160, H = 96;
    auto target = test::make_target(W, H);
    r.set_settings(s);
    Shot out;
    const std::uint64_t indirect = dev.stats().indirect_draws;
    for (int f = 0; f < frames; ++f) {
        r.begin(Camera3D::look_at({0, 1, 8}, {0, 0, 0}), DirectionalLight{}, {});
        scene.submit(r, hidden_grid);
        auto cmd = dev.begin_commands();
        REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    }
    out.px = dev.read_texture(target).value();
    out.stats = r.stats();
    out.survivors = r.read_occlusion_survivors().value();
    out.indirect_draws = dev.stats().indirect_draws - indirect;
    dev.destroy(target);
    return out;
}

} // namespace

TEST_CASE("gpu occlusion culling: hidden instances are rejected, image unchanged") {
    VICTUS_REQUIRE_COMPUTE_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    SceneRenderer& r = *sr.value();
    WallScene scene{ctx.create_mesh(make_cube(), "occlusion_cube").value(), {}, {}, {}};
    scene.wall.base_color = {0.8f, 0.2f, 0.2f, 1};
    scene.box.base_color = {0.2f, 0.8f, 0.2f, 1};
    scene.front.base_color = {0.2f, 0.3f, 0.9f, 1};

    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    s.depth_prepass = true;
    REQUIRE(s.occlusion_culling);
    const Shot on = shoot(r, s, scene);
    test::save_output("occlusion_on", 160, 96, on.px);
    CHECK(on.stats.occlusion);
    CHECK(on.stats.visible == 19);
    CHECK(on.stats.occlusion_tested == 19);
    CHECK(on.stats.occlusion_batches == on.stats.main_batches);
    CHECK(on.indirect_draws == on.stats.main_batches);
    CHECK(on.survivors == 3); // the wall and the two cubes in front of it

    RenderSettings off = s;
    off.occlusion_culling = false;
    const Shot ref = shoot(r, off, scene);
    CHECK_FALSE(ref.stats.occlusion);
    CHECK(ref.stats.occlusion_batches == 0);
    CHECK(ref.indirect_draws == 0);
    CHECK(ref.survivors == 0);
    CHECK(ref.stats.visible == on.stats.visible);
    CHECK(on.px == ref.px);

    // The hidden grid costs nothing on screen: identical to a scene without it.
    const Shot without = shoot(r, off, scene, 1, false);
    CHECK(without.px == on.px);

    // TAA jitters the projection; the cull pass uses the same jittered matrix as the pre-pass.
    RenderSettings taa = s;
    taa.taa = true;
    const Shot taa_on = shoot(r, taa, scene, 3);
    RenderSettings taa_off = taa;
    taa_off.occlusion_culling = false;
    auto sr2 = SceneRenderer::create(ctx);
    REQUIRE(sr2.ok());
    const Shot taa_ref = shoot(*sr2.value(), taa_off, scene, 3);
    CHECK(taa_on.stats.taa);
    CHECK(taa_on.survivors == 3);
    CHECK(taa_on.px == taa_ref.px);

    // A low wall: the upper rows of the grid peek over it. Partially visible instances survive
    // and the image still matches the unculled one.
    WallScene low = scene;
    low.wall_height = 1.6f;
    const Shot low_on = shoot(r, s, low);
    const Shot low_off = shoot(r, off, low);
    test::save_output("occlusion_low_wall", 160, 96, low_on.px);
    CHECK(low_on.survivors > 3);
    CHECK(low_on.survivors < 19);
    CHECK(low_on.px == low_off.px);

    // No pre-pass, no Hi-Z: occlusion culling stays off and every batch is drawn directly.
    RenderSettings no_prepass = s;
    no_prepass.depth_prepass = false;
    no_prepass.ssao = false;
    const Shot direct = shoot(r, no_prepass, scene);
    CHECK_FALSE(direct.stats.occlusion);
    CHECK(direct.indirect_draws == 0);
    ctx.destroy(scene.cube);
}
