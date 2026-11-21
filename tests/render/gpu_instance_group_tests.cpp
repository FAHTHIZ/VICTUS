// SPDX-License-Identifier: Apache-2.0
// Retained instance groups (ADR-0162): copies registered once, culled per cluster, drawn from a
// persistent GPU buffer. The image must match the per-frame submit_instances path, nothing is
// re-uploaded per frame, off-screen clusters are culled and ids are generation-checked.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cstdlib>
#include <limits>
#include <vector>

using namespace victus;
using namespace victus::render;
using victus::math::Mat4;
using victus::math::Vec3;
using victus::test::gpu;

namespace {

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
};

template <typename F>
Shot shoot(SceneRenderer& r, const Camera3D& cam, bool occlusion, F&& content, const char* save = nullptr) {
    auto& dev = gpu().ctx->device();
    constexpr std::uint32_t W = 160, H = 96;
    auto target = test::make_target(W, H);
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.ssao = true;     // depth pre-pass draws the group batches too
    s.taa = false;     // exact frames
    s.occlusion_culling = occlusion;
    r.set_settings(s);
    DirectionalLight sun;
    sun.shadow_distance = 40;
    r.begin(cam, sun, {});
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    Shot out{dev.read_texture(target).value(), r.stats()};
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

int max_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

std::vector<Mat4> grid(int side, float spacing) {
    std::vector<Mat4> out;
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x)
            out.push_back(Mat4::translation({(static_cast<float>(x) - static_cast<float>(side) * 0.5f) * spacing, 0.5f,
                                             (static_cast<float>(z) - static_cast<float>(side) * 0.5f) * spacing}) *
                          Mat4::scale({0.6f, 0.6f + 0.02f * static_cast<float>((x * 7 + z * 3) % 20), 0.6f}));
    return out;
}

} // namespace

TEST_CASE("gpu instance group: same image as per-frame instances, no per-frame upload") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "group_cube").value();
    auto plane = ctx.create_mesh(make_plane({60, 60}), "group_plane").value();
    Material m;
    m.base_color = {0.8f, 0.5f, 0.3f, 1};
    Material ground;
    const std::vector<Mat4> models = grid(20, 1.5f); // 400 copies
    const Camera3D cam = Camera3D::look_at({0, 12, 18}, {0, 0, 0});

    SceneRenderer::InstanceGroupDesc desc;
    desc.mesh = &cube;
    desc.material = m;
    desc.models = models;
    desc.cluster_size = 64;
    auto id = r.create_instance_group(desc);
    REQUIRE(id.ok());
    CHECK(r.instance_group_clusters(id.value()) == 7); // ceil(400 / 64)

    for (bool occlusion : {false, true}) {
        CAPTURE(occlusion);
        const Shot reference = shoot(r, cam, occlusion, [&](SceneRenderer& s) {
            s.submit(plane, ground, Mat4::identity());
            s.submit_instances(cube, m, models);
        });
        const Shot grouped = shoot(r, cam, occlusion, [&](SceneRenderer& s) {
            s.submit(plane, ground, Mat4::identity());
            s.submit_instance_group(id.value());
        }, occlusion ? "instance_group_occlusion" : "instance_group");
        // Second frame: the group buffer was written by the first; nothing new is uploaded.
        const Shot again = shoot(r, cam, occlusion, [&](SceneRenderer& s) {
            s.submit(plane, ground, Mat4::identity());
            s.submit_instance_group(id.value());
        });
        CHECK(max_diff(reference.px, grouped.px) <= 2);
        CHECK(max_diff(grouped.px, again.px) == 0);
        CHECK(reference.stats.submitted == grouped.stats.submitted);
        CHECK(grouped.stats.group_clusters == 7);
        CHECK(grouped.stats.group_clusters_visible > 0);
        CHECK(grouped.stats.group_instances_drawn <= 400);
        // Only the plane goes through the per-frame instance buffer (main + shadow passes).
        CHECK(grouped.stats.instances_uploaded < 8);
        CHECK(reference.stats.instances_uploaded > 400);
        CHECK(grouped.stats.main_batches <= 1 + 7);
        if (occlusion) CHECK(grouped.stats.occlusion);
    }
    r.destroy_instance_group(id.value());
    ctx.destroy(cube);
    ctx.destroy(plane);
}

TEST_CASE("gpu instance group: clusters outside the view are culled") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "group_cull_cube").value();
    const std::vector<Mat4> models = grid(64, 3.0f); // 4096 copies over ~190 m
    SceneRenderer::InstanceGroupDesc desc;
    desc.mesh = &cube;
    desc.models = models;
    auto id = r.create_instance_group(desc);
    REQUIRE(id.ok());
    const std::uint32_t clusters = r.instance_group_clusters(id.value());
    CHECK(clusters == 64);
    // A narrow view near one corner sees a few clusters only.
    Camera3D cam = Camera3D::look_at({-90, 3, -90}, {-80, 0, -80});
    cam.z_far = 40;
    const Shot near = shoot(r, cam, false, [&](SceneRenderer& s) { s.submit_instance_group(id.value()); });
    CHECK(near.stats.group_clusters == clusters);
    CHECK(near.stats.group_clusters_visible > 0);
    CHECK(near.stats.group_clusters_visible < clusters / 4);
    CHECK(near.stats.group_instances_drawn < 4096 / 4);
    CHECK(near.stats.instances_uploaded == 0);
    r.destroy_instance_group(id.value());
    ctx.destroy(cube);
}

TEST_CASE("gpu instance group: ids, validation and destruction") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "group_id_cube").value();
    const std::vector<Mat4> models = grid(4, 2.0f);

    SceneRenderer::InstanceGroupDesc bad;
    CHECK(!r.create_instance_group(bad).ok()); // no mesh
    bad.mesh = &cube;
    CHECK(!r.create_instance_group(bad).ok()); // no instances
    bad.models = models;
    const std::vector<math::Vec2> offsets(3);
    bad.lightmap_offsets = offsets;
    CHECK(!r.create_instance_group(bad).ok()); // offsets count mismatch
    std::vector<Mat4> nan_models = models;
    nan_models[2].c[3][0] = std::numeric_limits<float>::quiet_NaN();
    SceneRenderer::InstanceGroupDesc nan_desc;
    nan_desc.mesh = &cube;
    nan_desc.models = nan_models;
    CHECK(!r.create_instance_group(nan_desc).ok());

    SceneRenderer::InstanceGroupDesc desc;
    desc.mesh = &cube;
    desc.models = models;
    desc.cluster_size = 5;
    const std::uint32_t a = r.create_instance_group(desc).value();
    CHECK(r.instance_group_clusters(a) == 4); // ceil(16 / 5)
    r.destroy_instance_group(a);
    CHECK(r.instance_group_clusters(a) == 0);
    const std::uint32_t b = r.create_instance_group(desc).value();
    CHECK(b != a); // same slot, new generation: the stale id stays dead
    CHECK(r.instance_group_clusters(a) == 0);
    CHECK(r.instance_group_clusters(b) == 4);
    const Camera3D cam = Camera3D::look_at({0, 6, 8}, {0, 0, 0});
    const Shot stale = shoot(r, cam, false, [&](SceneRenderer& s) {
        s.submit_instance_group(a); // ignored
        s.submit_instance_group(0); // ignored
        s.submit_instance_group(0xFFFFFFFFu); // ignored
    });
    CHECK(stale.stats.group_clusters == 0);
    CHECK(stale.stats.submitted == 0);
    r.destroy_instance_group(b);
    r.destroy_instance_group(b); // twice: no effect
    ctx.destroy(cube);
}
