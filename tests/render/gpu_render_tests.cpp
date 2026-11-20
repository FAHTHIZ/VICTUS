// SPDX-License-Identifier: Apache-2.0
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"

#include <cstdlib>
#include <cstring>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

TEST_CASE("gpu: clear and readback") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& dev = *gpu().device;
    auto target = test::make_target(32, 16);
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {1.0f, 0.5f, 0.0f, 1.0f}, {}});
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    REQUIRE(px->size() == 32 * 16 * 4);
    auto p = pixel(px.value(), 32, 31, 15);
    CHECK(channel(p, 0) == 255);
    CHECK(channel(p, 1) >= 127);
    CHECK(channel(p, 1) <= 128);
    CHECK(channel(p, 2) == 0);
    dev.destroy(target);
}

TEST_CASE("gpu: sprite batch draws sorted, batched sprites") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    const std::uint32_t W = 128, H = 96;
    auto target = test::make_target(W, H);
    auto batch = SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    Image checker = make_checker(16, 2, 0xFF00FF00u, 0xFF00FF00u); // solid green
    auto tex = ctx.create_texture(checker, {false, false, "green"});
    REQUIRE(tex.ok());

    Camera2D cam;
    cam.pixels_per_unit = 1.0f; // 1 world unit == 1 pixel
    auto& sb = *batch.value();
    sb.begin(cam.view_projection(static_cast<float>(W), static_cast<float>(H)));
    // Layer 1 red square in the centre is drawn over a layer 0 green textured square.
    sb.draw_rect({0, 0}, {20, 20}, {1, 0, 0, 1}, 1);
    Sprite s;
    s.texture = tex.value();
    s.size = {40, 40};
    s.layer = 0;
    sb.draw(s);
    // A white rect at the top-left in world space (Y-up => top of the image).
    sb.draw_rect({-54, 38}, {8, 8}, {1, 1, 1, 1}, 0);
    sb.draw_line({-60, -40}, {60, -40}, 2.0f, {0, 0, 1, 1}, 2);

    auto cmd = dev.begin_commands();
    REQUIRE(sb.prepare(cmd).ok());
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
    auto pass = cmd.begin_render_pass(rp);
    pass.set_viewport({0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1});
    sb.render(pass, cmd);
    pass.end();
    cmd.submit_and_wait();
    CHECK(sb.stats().sprites == 4);
    // Runs: layer0 [white, green], layer1 [white], layer2 [white] -> adjacent equal textures merge.
    CHECK(sb.stats().batches == 3);
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("sprite_batch", W, H, px.value());
    auto centre = pixel(px.value(), W, W / 2, H / 2);
    CHECK(channel(centre, 0) == 255);
    CHECK(channel(centre, 1) == 0);
    auto green = pixel(px.value(), W, W / 2 + 15, H / 2);
    CHECK(channel(green, 1) == 255);
    CHECK(channel(green, 0) == 0);
    auto white = pixel(px.value(), W, 10, 10); // world (-54, 38) => pixel (10, 10)
    CHECK(white == 0xFFFFFFFFu);
    auto blue = pixel(px.value(), W, W / 2, H / 2 + 40);
    CHECK(channel(blue, 2) == 255);
    auto bg = pixel(px.value(), W, 2, H - 2);
    CHECK((bg & 0x00FFFFFFu) == 0u);
    dev.destroy(tex.value());
    dev.destroy(target);
}

TEST_CASE("gpu: sprite batch shows sRGB textures at their authored colours") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    const std::uint32_t W = 96, H = 32;
    // Authored PNG colour (77, 133, 230): imported as sRGB, sampled as linear by the GPU.
    const std::uint32_t authored = 0xFFE6854Du;
    Image img = make_checker(8, 2, authored, authored);
    auto srgb = ctx.create_texture(img, {true, false, "srgb"});
    auto unorm = ctx.create_texture(img, {false, false, "unorm"});
    REQUIRE(srgb.ok());
    REQUIRE(unorm.ok());
    for (const bool lit : {false, true}) {
        CAPTURE(lit);
        auto batch = SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
        REQUIRE(batch.ok());
        auto target = test::make_target(W, H);
        Camera2D cam;
        cam.pixels_per_unit = 1.0f;
        auto& sb = *batch.value();
        sb.begin(cam.view_projection(static_cast<float>(W), static_cast<float>(H)));
        if (lit) sb.set_lighting({1, 1, 1, 1});
        Sprite a;
        a.texture = srgb.value();
        a.size = {30, 30};
        a.position = {-32, 0};
        a.lit = lit;
        sb.draw(a);
        Sprite b = a;
        b.texture = unorm.value();
        b.position = {0, 0};
        sb.draw(b);
        Sprite c = a; // a half tint halves the display value
        c.position = {32, 0};
        c.color = {0.5f, 0.5f, 0.5f, 1};
        sb.draw(c);
        auto cmd = dev.begin_commands();
        REQUIRE(sb.prepare(cmd).ok());
        REQUIRE(sb.render_lights(cmd, W, H).ok());
        rhi::RenderPassDesc rp;
        rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
        auto pass = cmd.begin_render_pass(rp);
        pass.set_viewport({0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1});
        sb.render(pass, cmd);
        pass.end();
        cmd.submit_and_wait();
        auto px = dev.read_texture(target);
        REQUIRE(px.ok());
        const auto near = [](std::uint32_t p, int r, int g, int bl) {
            return std::abs(int(channel(p, 0)) - r) <= 2 && std::abs(int(channel(p, 1)) - g) <= 2 &&
                   std::abs(int(channel(p, 2)) - bl) <= 2;
        };
        const auto s = pixel(px.value(), W, W / 2 - 32, H / 2);
        const auto u = pixel(px.value(), W, W / 2, H / 2);
        const auto h = pixel(px.value(), W, W / 2 + 32, H / 2);
        CHECK(near(s, 77, 133, 230));
        CHECK(near(u, 77, 133, 230));
        CHECK(near(h, 38, 66, 115));
        dev.destroy(target);
    }
    dev.destroy(srgb.value());
    dev.destroy(unorm.value());
}

TEST_CASE("gpu: 3D PBR scene with shadows, sky and culling") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    const std::uint32_t W = 256, H = 160;
    auto target = test::make_target(W, H);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto& r = *sr.value();
    auto cube = ctx.create_mesh(make_cube(), "cube");
    auto plane = ctx.create_mesh(make_plane({20, 20}), "plane");
    REQUIRE(cube.ok());
    REQUIRE(plane.ok());

    Camera3D cam = Camera3D::look_at({0, 4, 8}, {0, 0.5f, 0});
    DirectionalLight sun;
    sun.direction = {0.0f, -1.0f, 0.0f}; // straight down: the cube's shadow lies directly below it
    sun.shadow_resolution = 1024;
    sun.shadow_distance = 20.0f;
    Environment env;
    Material ground;
    ground.base_color = {0.8f, 0.8f, 0.8f, 1};
    ground.roughness = 0.9f;
    Material red;
    red.base_color = {0.9f, 0.05f, 0.05f, 1};
    red.roughness = 0.4f;

    r.begin(cam, sun, env);
    r.submit(plane.value(), ground, math::Mat4::identity());
    r.submit(cube.value(), red, math::Mat4::trs({0, 1.5f, 0}, {}, {2, 1, 2}));
    r.submit(cube.value(), red, math::Mat4::translation({0, 0, 60})); // behind camera: culled
    r.add_point_light({{3, 1, 2}, {0.2f, 0.4f, 1.0f, 1}, 20.0f, 8.0f});
    r.grid(5, 1, {1, 1, 1, 0.3f});
    auto cmd = dev.begin_commands();
    auto st = r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
    cmd.submit_and_wait();

    CHECK(r.stats().submitted == 3);
    CHECK(r.stats().culled == 1);
    CHECK(r.stats().visible == 2);
    CHECK(r.stats().lines == 220); // 11 lines per axis, each split into 10 one-unit segments
    // High tier: shadow, prepass, occlusion cull, 2x SSAO, forward, 9x bloom, tonemap, FXAA. The
    // occlusion cull is a compute pass, so a device without compute (GLES) runs one fewer.
    CHECK(r.graph_info().executed_passes.size() == (dev.capabilities().compute ? 17u : 16u));

    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("scene_3d", W, H, px.value());
    // Project world points to pixels to probe the image.
    const math::Mat4 vp = cam.projection(static_cast<float>(W) / H) * cam.view;
    auto probe = [&](math::Vec3 p) {
        math::Vec4 c = vp * math::Vec4(p, 1.0f);
        float x = (c.x / c.w * 0.5f + 0.5f) * W, y = (0.5f - c.y / c.w * 0.5f) * H;
        return pixel(px.value(), W, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    };
    auto lum = [](std::uint32_t p) { return channel(p, 0) + channel(p, 1) + channel(p, 2); };
    const auto cube_top = probe({0, 2.0f, 0});
    CHECK(channel(cube_top, 0) > 2 * channel(cube_top, 1)); // red surface
    const auto lit_ground = probe({-4.5f, 0, 2.5f});
    const auto shadowed_ground = probe({0.5f, 0, 0.5f}); // inside the cube's footprint: in its shadow
    MESSAGE("lit ground lum " << lum(lit_ground) << " shadowed " << lum(shadowed_ground));
    CHECK(lum(lit_ground) > lum(shadowed_ground) + 60);
    const auto sky = pixel(px.value(), W, W / 2, 2);
    CHECK(channel(sky, 2) > channel(sky, 0)); // blue-ish sky at the top

    // Second frame reuses pooled transient textures instead of allocating new ones.
    const auto textures_before = dev.stats().live_textures;
    r.begin(cam, sun, env);
    r.submit(cube.value(), red, math::Mat4::identity());
    auto cmd2 = dev.begin_commands();
    REQUIRE(r.render(cmd2, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd2.submit_and_wait();
    CHECK(dev.stats().live_textures == textures_before);

    ctx.destroy(cube.value());
    ctx.destroy(plane.value());
    dev.destroy(target);
}

TEST_CASE("gpu: update_mesh rewrites in place while the data fits and regrows when it does not") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto made = ctx.create_mesh(make_cube(), "dyn");
    REQUIRE(made.ok());
    GpuMesh m = made.value();
    const auto vb0 = m.vertices, ib0 = m.indices;
    const auto live0 = dev.stats().live_buffers;
    CHECK(m.vertex_capacity == 24);
    CHECK(m.index_capacity == 36);

    // Smaller: same buffers, new counts, contents and bounds.
    const MeshData quad = make_plane({4, 2});
    REQUIRE(ctx.update_mesh(m, quad, "dyn").ok());
    CHECK(m.vertices == vb0);
    CHECK(m.indices == ib0);
    CHECK(m.vertex_count == quad.vertices.size());
    CHECK(m.index_count == quad.indices.size());
    CHECK(m.lods[0].index_count == quad.indices.size());
    CHECK(m.bounds.max.x == doctest::Approx(2.0f));
    CHECK(dev.stats().live_buffers == live0);
    auto vbytes = dev.read_buffer(m.vertices, 0, static_cast<std::uint32_t>(quad.vertices.size() * sizeof(Vertex3D)));
    REQUIRE(vbytes.ok());
    CHECK(std::memcmp(vbytes->data(), quad.vertices.data(), vbytes->size()) == 0);
    auto ibytes = dev.read_buffer(m.indices, 0, static_cast<std::uint32_t>(quad.indices.size() * sizeof(std::uint32_t)));
    REQUIRE(ibytes.ok());
    CHECK(std::memcmp(ibytes->data(), quad.indices.data(), ibytes->size()) == 0);

    // Larger than the buffers: regrown with headroom, then rewritten in place again.
    const MeshData big = make_plane({4, 4}, 8); // 81 vertices, 384 indices
    REQUIRE(ctx.update_mesh(m, big, "dyn").ok());
    CHECK(m.vertex_capacity >= big.vertices.size() + big.vertices.size() / 2);
    CHECK(m.index_capacity >= big.indices.size() + big.indices.size() / 2);
    CHECK(dev.stats().live_buffers == live0);
    const auto vb1 = m.vertices;
    REQUIRE(ctx.update_mesh(m, make_cube(), "dyn").ok());
    CHECK(m.vertices == vb1);
    CHECK(m.index_count == 36);

    // Empty data is refused and leaves the mesh drawable.
    CHECK_FALSE(ctx.update_mesh(m, MeshData{}, "dyn").ok());
    CHECK(m.valid());
    CHECK(m.index_count == 36);
    ctx.destroy(m);
    CHECK(dev.stats().live_buffers == live0 - 2);
}

TEST_CASE("gpu: the device asks only for features Mali-G52-class phones have") {
    // SDL refuses a Vulkan device outright when a requested optional feature is missing; Mali-G52
    // drivers (e.g. Infinix Hot 10S) lack depthClamp and shaderClipDistance, so neither is requested.
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const auto& caps = gpu().ctx->device().capabilities();
    CHECK_FALSE(caps.depth_clamp);
}
