// SPDX-License-Identifier: Apache-2.0
// Renderer benchmarks. CPU cases measure engine-side cost; "frame" cases include GPU execution
// (submit_and_wait) and therefore depend on the driver (lavapipe numbers are software rendering).
#include "bench_harness.hpp"

#include "victus/platform/platform.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"

#include <cstdio>
#include <cstdlib>

using namespace victus;
using namespace victus::math;

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    {
        h.run("render_graph/compile_64_passes", 64, [&] {
            render::RenderGraph g;
            rhi::TextureDesc d;
            d.format = rhi::Format::RGBA16_FLOAT;
            d.width = d.height = 512;
            d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
            auto out = g.import_texture("out", {1, 1}, d);
            render::RGTexture prev = g.create_texture("t0", d);
            g.add_pass("p0", [&](render::PassBuilder& b) { b.write_color(prev); }, nullptr);
            for (int i = 1; i < 63; ++i) {
                auto next = g.create_texture("t", d);
                g.add_pass("p", [&](render::PassBuilder& b) { b.read(prev); b.write_color(next); }, nullptr);
                prev = next;
            }
            g.add_pass("final", [&](render::PassBuilder& b) { b.read(prev); b.write_color(out); }, nullptr);
            g.mark_output(out);
            if (!g.compile().ok()) std::abort();
        });
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    if (!std::getenv("SDL_VIDEO_DRIVER") && !std::getenv("DISPLAY")) setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
#endif
    platform::PlatformDesc pd;
    pd.gamepad = false;
    auto plat = platform::Platform::create(pd);
    auto dev = plat ? rhi::Device::create({false, true, {}, false}) : Result<std::unique_ptr<rhi::Device>>(plat.error());
    if (!dev) {
        std::fprintf(stderr, "no GPU device (%s); GPU cases skipped\n", dev.error().message.c_str());
        return h.finish();
    }
    auto ctx = render::GpuContext::create(*dev.value()).value();
    auto& d = *dev.value();
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = 1280;
    td.height = 720;
    td.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    auto target = d.create_texture(td).value();

    {
        auto batch = render::SpriteBatch::create(*ctx, td.format).value();
        auto tex_a = ctx->create_texture(render::make_checker(32, 2, ~0u, 0xFF000000u), {false, false, "a"}).value();
        auto tex_b = ctx->create_texture(render::make_checker(32, 4, ~0u, 0xFF0000FFu), {false, false, "b"}).value();
        constexpr int kSprites = 20000;
        auto fill = [&] {
            batch->begin(render::Camera2D{}.view_projection(1280, 720));
            for (int i = 0; i < kSprites; ++i) {
                render::Sprite s;
                s.texture = (i & 1) ? tex_a : tex_b;
                s.position = {static_cast<float>(i % 200) * 6.0f - 600.0f, static_cast<float>(i / 200) * 6.0f - 300.0f};
                s.size = {5, 5};
                s.rotation = static_cast<float>(i) * 0.01f;
                s.layer = i % 4;
                batch->draw(s);
            }
        };
        h.run("sprite_batch/build_upload_20k", kSprites, [&] {
            fill();
            auto cmd = d.begin_commands();
            if (!batch->prepare(cmd).ok()) std::abort();
            cmd.submit();
        });
        h.run("sprite_batch/frame_20k_gpu", kSprites, [&] {
            fill();
            auto cmd = d.begin_commands();
            if (!batch->prepare(cmd).ok()) std::abort();
            rhi::RenderPassDesc rp;
            rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
            auto pass = cmd.begin_render_pass(rp);
            pass.set_viewport({0, 0, 1280, 720, 0, 1});
            batch->render(pass, cmd);
            pass.end();
            cmd.submit_and_wait();
        });
        std::printf("sprite batches for 20k sprites over 2 textures x 4 layers: %u\n", batch->stats().batches);
        d.destroy(tex_a);
        d.destroy(tex_b);
    }
    {
        auto scene = render::SceneRenderer::create(*ctx).value();
        auto cube = ctx->create_mesh(render::make_cube(), "cube").value();
        auto plane = ctx->create_mesh(render::make_plane({200, 200}), "plane").value();
        render::Camera3D cam = render::Camera3D::look_at({0, 30, 60}, {0, 0, 0});
        cam.z_far = 400;
        render::DirectionalLight sun;
        sun.shadow_distance = 120;
        render::Material m;
        constexpr int kSide = 32; // 1024 cubes
        auto submit_all = [&] {
            scene->begin(cam, sun, {});
            scene->submit(plane, m, Mat4::identity());
            for (int z = 0; z < kSide; ++z)
                for (int x = 0; x < kSide; ++x)
                    scene->submit(cube, m, Mat4::translation({(x - kSide / 2) * 3.0f, 0.5f, (z - kSide / 2) * 3.0f}));
        };
        h.run("scene/frame_1024_cubes_720p_gpu", kSide * kSide, [&] {
            submit_all();
            auto cmd = d.begin_commands();
            if (!scene->render(cmd, target, 1280, 720, td.format).ok()) std::abort();
            cmd.submit_and_wait();
        });
        std::printf("scene: visible %u culled %u casters %u\n", scene->stats().visible, scene->stats().culled,
                    scene->stats().shadow_casters);
        ctx->destroy(cube);
        ctx->destroy(plane);
    }
    d.destroy(target);
    ctx.reset();
    return h.finish();
}
