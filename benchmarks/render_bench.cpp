// SPDX-License-Identifier: Apache-2.0
// Renderer benchmarks. CPU cases measure engine-side cost; "frame" cases include GPU execution
// (submit_and_wait) and therefore depend on the driver (lavapipe numbers are software rendering).
#include "bench_harness.hpp"

#include "victus/platform/platform.hpp"
#include "victus/assets/reference_rigs.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/profiler.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/skinning.hpp"
#include "victus/render/sprite_batch.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
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
                    scene->submit(cube, m, Mat4::translation({static_cast<float>(x - kSide / 2) * 3.0f, 0.5f, static_cast<float>(z - kSide / 2) * 3.0f}));
        };
        h.run("scene/frame_1024_cubes_720p_gpu", kSide * kSide, [&] {
            submit_all();
            auto cmd = d.begin_commands();
            if (!scene->render(cmd, target, 1280, 720, td.format).ok()) std::abort();
            cmd.submit_and_wait();
        });
        std::printf("scene: visible %u culled %u casters %u\n", scene->stats().visible, scene->stats().culled,
                    scene->stats().shadow_casters);
        std::printf("scene: main batches %u shadow batches %u\n", scene->stats().main_batches,
                    scene->stats().shadow_batches);

        // Instancing A/B: 4096 identical cubes, low tier at a small resolution so draw submission
        // (not fill rate) dominates.
        auto small = d.create_texture([&] {
            rhi::TextureDesc sd = td;
            sd.name = "bench_small";
            sd.width = 320;
            sd.height = 180;
            return sd;
        }()).value();
        constexpr int kBig = 64;
        render::Camera3D far_cam = render::Camera3D::look_at({0, 90, 140}, {0, 0, 0});
        far_cam.z_far = 600;
        render::DirectionalLight far_sun;
        far_sun.shadow_distance = 300;
        for (bool instancing : {true, false}) {
            render::RenderSettings rs = render::RenderSettings::preset(render::QualityTier::Low);
            rs.instancing = instancing;
            scene->set_settings(rs);
            h.run(instancing ? "scene/frame_4096_cubes_low_320p_instanced_gpu" : "scene/frame_4096_cubes_low_320p_unbatched_gpu",
                  kBig * kBig, [&] {
                      scene->begin(far_cam, far_sun, {});
                      for (int z = 0; z < kBig; ++z)
                          for (int x = 0; x < kBig; ++x)
                              scene->submit(cube, m, Mat4::translation({static_cast<float>(x - kBig / 2) * 3.0f, 0.5f, static_cast<float>(z - kBig / 2) * 3.0f}));
                      auto cmd = d.begin_commands();
                      if (!scene->render(cmd, small, 320, 180, td.format).ok()) std::abort();
                      cmd.submit_and_wait();
                  });
            std::printf("instancing %s: visible %u, main batches %u, shadow batches %u\n", instancing ? "on" : "off",
                        scene->stats().visible, scene->stats().main_batches, scene->stats().shadow_batches);
        }
        // Retained instance groups (ADR-0162) vs per-frame instances: 16,384 cubes, Low tier, tiny
        // target so vertex/fill work stays small. "cpu" is the render() call alone (culling, LOD,
        // batching, upload recording); the frame cases add GPU execution and wait.
        {
            constexpr int kCity = 128;
            std::vector<Mat4> city;
            city.reserve(kCity * kCity);
            for (int z = 0; z < kCity; ++z)
                for (int x = 0; x < kCity; ++x)
                    city.push_back(Mat4::translation({static_cast<float>(x - kCity / 2) * 3.0f, 0.5f, static_cast<float>(z - kCity / 2) * 3.0f}));
            render::Camera3D city_cam = render::Camera3D::look_at({0, 40, 200}, {0, 0, 0});
            city_cam.z_far = 800;
            render::DirectionalLight city_sun;
            city_sun.shadow_distance = 300;
            render::RenderSettings rs = render::RenderSettings::preset(render::QualityTier::Low);
            scene->set_settings(rs);
            render::SceneRenderer::InstanceGroupDesc gd;
            gd.mesh = &cube;
            gd.material = m;
            gd.models = city;
            const auto t0 = std::chrono::steady_clock::now();
            const std::uint32_t group = scene->create_instance_group(gd).value();
            const double create_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            for (bool retained : {false, true}) {
                double cpu_ms = 0;
                int frames = 0;
                h.run(retained ? "scene/frame_16384_cubes_low_320p_group_gpu" : "scene/frame_16384_cubes_low_320p_per_frame_gpu",
                      kCity * kCity, [&] {
                          const auto c0 = std::chrono::steady_clock::now();
                          scene->begin(city_cam, city_sun, {});
                          if (retained)
                              scene->submit_instance_group(group);
                          else
                              scene->submit_instances(cube, m, city);
                          auto cmd = d.begin_commands();
                          if (!scene->render(cmd, small, 320, 180, td.format).ok()) std::abort();
                          cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
                          ++frames;
                          cmd.submit_and_wait();
                      });
                const auto& st = scene->stats();
                std::printf("16384 cubes %s: cpu submit+render %.3f ms/frame (%d frames), visible %u, instances uploaded %u, "
                            "main batches %u, shadow batches %u, group clusters %u visible %u drawn %u\n",
                            retained ? "retained group" : "per-frame", frames ? cpu_ms / frames : 0.0, frames, st.visible,
                            st.instances_uploaded, st.main_batches, st.shadow_batches, st.group_clusters,
                            st.group_clusters_visible, st.group_instances_drawn);
                // Where the CPU time goes: the renderer's sequential render.cpu.* stage zones,
                // averaged over 10 profiled frames (wall clock, main thread).
                const auto steady_state = [&](const char* label) {
                    constexpr int kProfiled = 10;
                    profiler::begin_capture();
                    double submit_ms = 0, render_ms = 0;
                    for (int f = 0; f < kProfiled; ++f) {
                        const std::uint64_t p0 = profiler::now_ns();
                        scene->begin(city_cam, city_sun, {});
                        if (retained)
                            scene->submit_instance_group(group);
                        else
                            scene->submit_instances(cube, m, city);
                        auto cmd = d.begin_commands();
                        const std::uint64_t p1 = profiler::now_ns();
                        if (!scene->render(cmd, small, 320, 180, td.format).ok()) std::abort();
                        const std::uint64_t p2 = profiler::now_ns();
                        submit_ms += static_cast<double>(p1 - p0) * 1e-6;
                        render_ms += static_cast<double>(p2 - p1) * 1e-6;
                        cmd.submit_and_wait();
                    }
                    std::map<std::string, double> stage_ms;
                    for (const auto& z : profiler::end_capture())
                        if (std::strncmp(z.name, "render.cpu.", 11) == 0) stage_ms[z.name] += static_cast<double>(z.end_ns - z.begin_ns) * 1e-6;
                    std::printf("16384 cubes %s steady state (ms/frame): begin+submit %.3f render %.3f; stages:",
                                label, submit_ms / kProfiled, render_ms / kProfiled);
                    for (const auto& [name, ms] : stage_ms) std::printf(" %s %.3f", name.c_str() + 11, ms / kProfiled);
                    std::printf("\n");
                };
                steady_state(retained ? "retained group" : "per-frame");
                if (!retained) {
                    // The same frames with per-draw visibility on the job system (ADR-0228).
                    jobs::JobSystem workers(0);
                    scene->set_job_system(&workers);
                    steady_state("per-frame jobs");
                    scene->set_job_system(nullptr);
                }
            }
            std::printf("instance group create (16384 copies, 256 clusters): %.3f ms\n", create_ms);
            scene->destroy_instance_group(group);
        }
        d.destroy(small);
        ctx->destroy(cube);
        ctx->destroy(plane);
    }
    {
        // Skinned characters (ADR-0207): 16 reference quadrupeds (4,516 vertices, 38 joints) posed
        // by the Walk clip at different times, drawn GPU-skinned with linear and dual quaternion
        // palettes, against the same poses pre-skinned on the CPU and drawn as static meshes (the
        // vertex shader's skinning cost is the difference). 640x360 High tier with shadows.
        auto scene = render::SceneRenderer::create(*ctx).value();
        const assets::CookedModel rig = assets::make_reference_rig(assets::ReferenceRig::Quadruped);
        const render::MeshData& md = rig.meshes[0].data;
        auto skinned = ctx->create_mesh(md, "quadruped").value();
        constexpr int kCount = 16;
        std::vector<std::vector<Mat4>> lbs(kCount), dqs(kCount);
        std::vector<render::GpuMesh> baked;
        std::vector<Mat4> place;
        for (int i = 0; i < kCount; ++i) {
            lbs[i] = assets::pose_palette(rig, assets::pose_world(rig, &rig.animations[0], 0.0625f * static_cast<float>(i)));
            dqs[i] = lbs[i];
            render::encode_dq_palette(dqs[i]);
            render::MeshData cpu = md;
            render::skin_vertices(md, lbs[i], render::SkinningMethod::Linear, cpu.vertices);
            cpu.skin.clear();
            baked.push_back(ctx->create_mesh(cpu, "quadruped_cpu").value());
            place.push_back(Mat4::translation({static_cast<float>(i % 4 - 2) * 1.2f, 0, static_cast<float>(i / 4 - 2) * 1.4f}));
        }
        auto mid = d.create_texture([&] {
            rhi::TextureDesc sd = td;
            sd.name = "bench_skin";
            sd.width = 640;
            sd.height = 360;
            return sd;
        }()).value();
        const render::Camera3D cam = render::Camera3D::look_at({0, 3.5f, 6}, {0, 0.4f, -0.6f});
        render::DirectionalLight sun;
        sun.shadow_distance = 20;
        render::Material m;
        scene->set_settings(render::RenderSettings::preset(render::QualityTier::High));
        const math::Aabb local{{-0.6f, -0.2f, -1.2f}, {0.6f, 1.4f, 1.0f}};
        for (int mode = 0; mode < 3; ++mode) {
            const char* name = mode == 0 ? "scene/frame_16_quadrupeds_640p_static_gpu"
                             : mode == 1 ? "scene/frame_16_quadrupeds_640p_skinned_lbs_gpu"
                                         : "scene/frame_16_quadrupeds_640p_skinned_dqs_gpu";
            h.run(name, kCount, [&] {
                scene->begin(cam, sun, {});
                for (int i = 0; i < kCount; ++i) {
                    if (mode == 0) scene->submit(baked[static_cast<std::size_t>(i)], m, place[static_cast<std::size_t>(i)]);
                    else
                        scene->submit_skinned(skinned, m, place[static_cast<std::size_t>(i)], mode == 1 ? lbs[static_cast<std::size_t>(i)] : dqs[static_cast<std::size_t>(i)],
                                              local.transformed(place[static_cast<std::size_t>(i)]));
                }
                auto cmd = d.begin_commands();
                if (!scene->render(cmd, mid, 640, 360, td.format).ok()) std::abort();
                cmd.submit_and_wait();
            });
        }
        std::printf("skinned quadrupeds: palette %zu B per instance, skin stream %zu B per mesh (%zu B/vertex)\n",
                    lbs[0].size() * sizeof(Mat4), md.skin.size() * sizeof(render::SkinVertex), sizeof(render::SkinVertex));
        for (auto& b : baked) ctx->destroy(b);
        ctx->destroy(skinned);
        d.destroy(mid);
    }
    d.destroy(target);
    ctx.reset();
    return h.finish();
}
