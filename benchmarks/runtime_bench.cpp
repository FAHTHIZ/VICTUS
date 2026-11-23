// SPDX-License-Identifier: Apache-2.0
// Runtime frame CPU cost for a mostly static scene of 10,000 mesh renderers (CPU frame
// architecture plan, docs/audits/CPU_FRAME_ARCHITECTURE_VS_UNITY_UNREAL.md): transform
// propagation, scene extraction (ECS -> SceneRenderer submissions) and SceneRenderer::render()
// recording, each timed separately on the main thread. The GPU work is submitted and waited
// outside the timed parts, so lavapipe's software rendering does not enter the numbers.
#include "bench_harness.hpp"

#include "victus/core/jobs.hpp"
#include "victus/platform/platform.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/scene/scene.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace victus;

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
#if !defined(_WIN32) && !defined(__APPLE__)
    if (!std::getenv("SDL_VIDEO_DRIVER") && !std::getenv("DISPLAY")) setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
#endif
    platform::PlatformDesc pd;
    pd.gamepad = false;
    auto plat = platform::Platform::create(pd);
    auto dev = plat ? rhi::Device::create({false, true, {}, false}) : Result<std::unique_ptr<rhi::Device>>(plat.error());
    if (!dev) {
        std::fprintf(stderr, "no GPU device (%s); runtime cases skipped\n", dev.error().message.c_str());
        return h.finish();
    }
    auto ctx = render::GpuContext::create(*dev.value()).value();
    auto& d = *dev.value();
    {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = 320;
        td.height = 180;
        td.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
        auto target = d.create_texture(td).value();
        auto renderer = render::SceneRenderer::create(*ctx).value();
        renderer->set_settings(render::RenderSettings::preset(render::QualityTier::Low));
        runtime::GpuAssetCache cache(*ctx, nullptr);

        // 10,000 cubes on a 100 x 100 grid, eight tints, a camera and a sun; nothing moves.
        scene::Scene s;
        const scene::Entity cam = s.create("Camera");
        s.world().get<scene::Transform>(cam)->position = {0, 40, 200};
        s.world().get<scene::Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-12));
        s.world().add<scene::CameraComponent>(cam).z_far = 800;
        const scene::Entity sun = s.create("Sun");
        s.world().get<scene::Transform>(sun)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-50));
        s.world().add<scene::LightComponent>(sun);
        constexpr int kSide = 100;
        for (int z = 0; z < kSide; ++z)
            for (int x = 0; x < kSide; ++x) {
                const scene::Entity e = s.create("Cube");
                s.world().get<scene::Transform>(e)->position = {static_cast<float>(x - kSide / 2) * 3.0f, 0.5f,
                                                                static_cast<float>(z - kSide / 2) * 3.0f};
                auto& mr = s.world().add<scene::MeshRendererComponent>(e);
                mr.primitive = "cube";
                const float t = static_cast<float>((x + z) % 8) / 8.0f;
                mr.base_color = {0.3f + 0.6f * t, 0.5f, 0.9f - 0.6f * t, 1.0f};
            }

        double transforms_ms = 0, extract_ms = 0, render_ms = 0;
        int frames = 0;
        runtime::ExtractStats es;
        const auto frame = [&] {
            const auto t0 = std::chrono::steady_clock::now();
            s.update_transforms();
            const auto t1 = std::chrono::steady_clock::now();
            es = runtime::extract_scene(s, cache, *renderer);
            const auto t2 = std::chrono::steady_clock::now();
            auto cmd = d.begin_commands();
            if (!renderer->render(cmd, target, 320, 180, td.format).ok()) std::abort();
            const auto t3 = std::chrono::steady_clock::now();
            cmd.submit_and_wait();
            transforms_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            extract_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
            render_ms += std::chrono::duration<double, std::milli>(t3 - t2).count();
            ++frames;
        };
        for (int i = 0; i < 3; ++i) frame(); // pipelines, primitive meshes, history
        const auto report = [&](const char* label) {
            std::printf("runtime 10k static %s (ms/frame over %d frames): transforms %.3f extract %.3f render %.3f total %.3f; "
                        "meshes %u, visible %u, main batches %u\n",
                        label, frames, transforms_ms / frames, extract_ms / frames, render_ms / frames,
                        (transforms_ms + extract_ms + render_ms) / frames, es.meshes, renderer->stats().visible,
                        renderer->stats().main_batches);
        };
        struct Variant {
            const char* name;
            const char* label;
            bool jobs, retain;
        };
        for (const Variant& v : {Variant{"runtime/frame_10k_static_cpu_gpu", "serial", false, false},
                                 Variant{"runtime/frame_10k_static_jobs_cpu_gpu", "jobs", true, false},
                                 Variant{"runtime/frame_10k_static_retained_cpu_gpu", "retained", false, true}}) {
            if (!h.selected(v.name)) continue;
            jobs::JobSystem workers(0);
            renderer->set_job_system(v.jobs ? &workers : nullptr);
            render::RenderSettings rs = render::RenderSettings::preset(render::QualityTier::Low);
            rs.retain_static_draws = v.retain; // ADR-0230
            renderer->set_settings(rs);
            for (int i = 0; i < 10; ++i) frame(); // promotes the unchanged renderers
            transforms_ms = extract_ms = render_ms = 0;
            frames = 0;
            h.run(v.name, kSide * kSide, frame);
            report(v.label);
            if (v.retain)
                std::printf("retained: %u objects in %u groups, %u rebuilds in the last frame\n", renderer->stats().static_retained,
                            renderer->stats().static_groups, renderer->stats().static_rebuilds);
            renderer->set_job_system(nullptr);
        }
        cache.clear();
        renderer.reset();
        d.destroy(target);
    }
    return h.finish();
}
