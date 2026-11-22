// SPDX-License-Identifier: Apache-2.0
// Overdraw debug view (roadmap 6b, Workstream H2): stacked planes seen from above give known
// per-pixel layer counts, the heat ramp shows them, and blended, alpha-tested, double-sided and
// back-facing surfaces count the way the main passes rasterise them.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 128, H = 96;

} // namespace

TEST_CASE("gpu overdraw: stacked layers are counted per pixel and shown as heat") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto plane = ctx.create_mesh(make_plane({4, 4}), "overdraw_plane").value();
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    s.fxaa = false;
    s.render_scale = 1.0f;
    s.debug_view = DebugView::Overdraw;
    sr.value()->set_settings(s);
    REQUIRE(sr.value()->settings().debug_view == DebugView::Overdraw);

    Camera3D cam = Camera3D::look_at({0, 10, 0}, {0, 0, 0}, {0, 0, -1});
    cam.fov_y = math::radians(60.0f);
    auto frame = [&](const auto& submit, const char* save = nullptr) {
        auto target = test::make_target(W, H);
        sr.value()->begin(cam, DirectionalLight{}, {});
        submit();
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target).value();
        if (save) test::save_output(save, W, H, px);
        dev.destroy(target);
        return px;
    };
    auto at = [](const std::vector<std::uint8_t>& px, std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (std::size_t{y} * W + x) * 4;
        return std::array<int, 3>{px[i], px[i + 1], px[i + 2]};
    };
    const auto layer = [](float y) { return math::Mat4::translation({0, y, 0}); };
    Material opaque;

    SUBCASE("three opaque planes give three layers in their overlap") {
        const auto px = frame(
            [&] {
                for (float y : {0.0f, 1.0f, 2.0f}) sr.value()->submit(plane, opaque, layer(y));
            },
            "overdraw_three_planes");
        auto st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        CHECK(st->width == W);
        CHECK(st->height == H);
        CHECK(st->max_layers == 3);
        CHECK(st->covered_pixels > 0);
        CHECK(st->covered_pixels < W * H);
        // The nearest plane (y = 2, 8 m away) covers the most; the farther ones lie inside it.
        INFO("covered " << st->covered_pixels << ", layers " << st->layers << ", average " << st->average());
        CHECK(st->average() > 1.5);
        CHECK(st->average() < 3.0);
        // Heat ramp: yellow at the centre (3 layers), black in the corner (0 layers).
        const auto centre = at(px, W / 2, H / 2), corner = at(px, 1, 1);
        CHECK(centre[0] > 200);
        CHECK(centre[1] > 180);
        CHECK(centre[2] < 80);
        CHECK(corner == std::array<int, 3>{0, 0, 0});
    }

    SUBCASE("blended and alpha-tested layers count; back faces only when double-sided") {
        Material blended;
        blended.blend = MaterialBlend::Alpha;
        blended.base_color = {1, 1, 1, 0.3f};
        Material cutout;
        cutout.alpha_cutoff = 0.5f;
        (void)frame([&] {
            sr.value()->submit(plane, opaque, layer(0.0f));
            sr.value()->submit(plane, cutout, layer(1.0f));
            sr.value()->submit(plane, blended, layer(2.0f));
        });
        auto st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        CHECK(st->max_layers == 3);

        // A plane flipped upside down faces away from the camera: culled unless double-sided.
        const math::Mat4 flipped = layer(1.0f) * math::Mat4::rotation(math::Quat::axis_angle({1, 0, 0}, math::radians(180.0f)));
        (void)frame([&] {
            sr.value()->submit(plane, opaque, layer(0.0f));
            sr.value()->submit(plane, opaque, flipped);
        });
        st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        CHECK(st->max_layers == 1);
        Material two_sided;
        two_sided.double_sided = true;
        (void)frame([&] {
            sr.value()->submit(plane, opaque, layer(0.0f));
            sr.value()->submit(plane, two_sided, flipped);
        });
        st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        CHECK(st->max_layers == 2);
    }

    SUBCASE("particle billboards and ribbons count, separately reported (ADR-0212)") {
        // Five billboards stacked 9 m from the camera over the floor plane.
        std::vector<ParticleInstance> stack(5);
        for (auto& p : stack) {
            p.position_size = {0, 1, 0, 2};
            p.color = {1, 0.5f, 0.2f, 0.4f};
        }
        const math::Aabb box{{-1.5f, 0, -1.5f}, {1.5f, 2, 1.5f}};
        (void)frame(
            [&] {
                sr.value()->submit(plane, opaque, layer(0.0f));
                sr.value()->submit_particles(stack, ParticleMaterial{}, box);
            },
            "overdraw_particles");
        auto st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        // Expected quad: 2 m at 9 m with a 60 degree vertical field of view on 96 rows.
        const double side = 2.0 / (2.0 * 9.0 * std::tan(math::radians(30.0f))) * H;
        INFO("particle pixels " << st->particle_pixels << " (quad about " << side * side << "), layers " << st->particle_layers
                                << ", fill " << st->particle_fill() << ", total max " << st->max_layers);
        CHECK(st->max_layers == 6);
        CHECK(st->particle_max_layers == 5);
        CHECK(st->particle_layers == 5ull * st->particle_pixels);
        CHECK(std::abs(static_cast<double>(st->particle_pixels) - side * side) < side * 4 + 8);
        CHECK(std::abs(st->particle_average() - 5.0) < 1e-9);
        CHECK(std::abs(st->particle_fill() - 5.0 * st->particle_pixels / (W * H)) < 1e-9);
        // Additive billboards count the same; a ribbon across the view adds one particle layer.
        ParticleMaterial add;
        add.blend = ParticleBlend::Additive;
        std::vector<RibbonPoint> ribbon(2);
        ribbon[0].position = {-3, 1.5f, 0};
        ribbon[1].position = {3, 1.5f, 0};
        for (auto& r : ribbon) {
            r.width = 0.5f;
            r.color = {1, 1, 1, 1};
        }
        const std::uint32_t strip = 2;
        (void)frame([&] {
            sr.value()->submit_particles(stack, add, box);
            sr.value()->submit_ribbons(ribbon, std::span(&strip, 1), add, math::Aabb{{-3.5f, 1, -0.5f}, {3.5f, 2, 0.5f}});
        });
        st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        CHECK(st->particle_max_layers == 6);
        CHECK(st->max_layers == 6);
        CHECK(st->particle_layers > 5ull * static_cast<std::uint64_t>(side * side * 0.8));
        // Without particles the green channel stays empty.
        (void)frame([&] { sr.value()->submit(plane, opaque, layer(0.0f)); });
        st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        CHECK(st->particle_pixels == 0);
        CHECK(st->particle_fill() == 0.0);
    }

    SUBCASE("turning the view off restores the lit image and empties the readback") {
        (void)frame([&] { sr.value()->submit(plane, opaque, layer(0.0f)); });
        s.debug_view = DebugView::None;
        sr.value()->set_settings(s);
        const auto px = frame([&] { sr.value()->submit(plane, opaque, layer(0.0f)); });
        CHECK(sr.value()->stats().main_batches == 1);
        auto st = sr.value()->read_overdraw();
        REQUIRE(st.ok());
        CHECK(st->covered_pixels == 0);
        CHECK(st->max_layers == 0);
        CHECK(at(px, 1, 1) != std::array<int, 3>{0, 0, 0}); // sky, not the heat ramp's black
    }

    // Out-of-range values sanitise to None.
    RenderSettings bad = s;
    bad.debug_view = static_cast<DebugView>(9);
    CHECK(bad.sanitized().debug_view == DebugView::None);
    ctx.destroy(plane);
}
