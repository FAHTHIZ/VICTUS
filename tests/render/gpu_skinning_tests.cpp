// SPDX-License-Identifier: Apache-2.0
// GPU skinning (Slice 12): skinned meshes follow their joint palettes in the main, depth
// pre-pass and shadow passes, skinned instances batch, and the runtime path (cooked model ->
// instantiate -> Animator -> extract_scene) poses the mesh from animated entities.
#include "../assets/skinned_model.hpp"
#include "gpu_fixture.hpp"
#include "skin_test_meshes.hpp"
#include "victus/render/skinning.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <chrono>
#include <cstdlib>
#include <numbers>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 192, H = 112;

math::Mat4 bend(float angle) {
    // Rotate the upper segment about the Spine joint at (0, 1, 0).
    return math::Mat4::translation({0, 1, 0}) * math::Mat4::rotation(math::Quat::axis_angle({0, 0, 1}, angle)) *
           math::Mat4::translation({0, -1, 0});
}

struct Shot {
    std::vector<std::uint8_t> px;
    SceneStats stats;
    Camera3D cam;
    bool red(math::Vec3 p) const {
        const math::Vec4 clip = cam.projection(float(W) / float(H)) * cam.view * math::Vec4(p, 1.0f);
        const float sx = (clip.x / clip.w * 0.5f + 0.5f) * float(W), sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * float(H);
        const auto c = test::pixel(px, W, static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
        return channel(c, 0) > 80 && channel(c, 0) > channel(c, 1) + 50;
    }
};

template <typename F>
Shot shoot(SceneRenderer& r, F&& content, bool instancing = true, const char* save = nullptr, math::Vec3 eye = {0, 1, 8}) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.ssao = true; // depth pre-pass
    s.instancing = instancing;
    s.taa = false; // compares exact frames; jitter would differ per frame
    r.set_settings(s);
    Shot out;
    out.cam = Camera3D::look_at(eye, {eye.x, 1, 0});
    DirectionalLight sun;
    sun.shadow_distance = 20;
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    r.begin(out.cam, sun, env);
    content(r);
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    out.px = dev.read_texture(target).value();
    out.stats = r.stats();
    if (save) test::save_output(save, W, H, out.px);
    dev.destroy(target);
    return out;
}

int max_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

Material column_material() {
    Material m;
    m.base_color = {0.9f, 0.1f, 0.05f, 1};
    m.unlit = true;
    return m;
}

} // namespace

TEST_CASE("gpu skinning: the mesh follows its joint palette in every pass") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    const assets::CookedModel model = test::make_skinned_column();
    auto mesh = ctx.create_mesh(model.meshes[0].data, "skin_column").value();
    REQUIRE(mesh.skinned());
    auto floor = ctx.create_mesh(make_plane({20, 20}), "skin_floor").value();
    const Material mat = column_material();
    const float a = std::numbers::pi_v<float> / 2;
    const math::Mat4 palette[2] = {math::Mat4::identity(), bend(a)};
    const math::Aabb bounds{{-1.3f, -0.3f, -0.3f}, {0.3f, 1.3f, 0.3f}};

    const Shot posed = shoot(*sr.value(), [&](SceneRenderer& r) {
        r.submit(floor, {}, math::Mat4::identity());
        r.submit_skinned(mesh, mat, math::Mat4::identity(), palette, bounds);
    }, true, "skinning_bent");
    CHECK(posed.stats.skinned_draws == 1);
    CHECK(posed.stats.joints_uploaded >= 2);
    CHECK(posed.stats.prepass_draws >= 2);
    CHECK(posed.stats.shadow_draws >= 2); // floor + skinned column in at least one cascade
    CHECK(posed.red({0, 0.5f, 0}));        // lower segment (joint 0) stays put
    CHECK(posed.red({-0.7f, 1.0f, 0.25f}));  // upper segment now points to -X
    CHECK_FALSE(posed.red({0, 1.75f, 0.25f}));

    // Rest palette (identity joints) matches the unskinned draw of the same mesh.
    const math::Mat4 rest[2] = {};
    const Shot rest_shot = shoot(*sr.value(), [&](SceneRenderer& r) {
        r.submit(floor, {}, math::Mat4::identity());
        r.submit_skinned(mesh, mat, math::Mat4::identity(), rest, model.meshes[0].data.bounds());
    });
    const Shot plain = shoot(*sr.value(), [&](SceneRenderer& r) {
        r.submit(floor, {}, math::Mat4::identity());
        r.submit(mesh, mat, math::Mat4::identity());
    });
    CHECK(plain.stats.skinned_draws == 0);
    CHECK(rest_shot.red({0, 1.75f, 0.25f}));
    CHECK(max_diff(rest_shot.px, plain.px) <= 2);
    // No joints: submit_skinned falls back to the static path.
    const Shot fallback = shoot(*sr.value(), [&](SceneRenderer& r) {
        r.submit(floor, {}, math::Mat4::identity());
        r.submit_skinned(mesh, mat, math::Mat4::identity(), {}, bounds);
    });
    CHECK(fallback.stats.skinned_draws == 0);
    CHECK(max_diff(fallback.px, plain.px) <= 2);
    ctx.destroy(mesh);
    ctx.destroy(floor);
}

TEST_CASE("gpu skinning: skinned instances with different poses share one batch") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto mesh = ctx.create_mesh(test::make_skinned_column().meshes[0].data, "skin_column_b").value();
    const Material mat = column_material();
    const float a = std::numbers::pi_v<float> / 2;
    const math::Mat4 left[2] = {math::Mat4::identity(), bend(a)};
    const math::Mat4 right[2] = {math::Mat4::identity(), bend(-a)};
    const math::Mat4 straight[2] = {};
    auto content = [&](SceneRenderer& r) {
        const math::Aabb box{{-1.3f, -0.3f, -0.3f}, {1.3f, 2.3f, 0.3f}};
        r.submit_skinned(mesh, mat, math::Mat4::translation({-3, 0, 0}), left, box.transformed(math::Mat4::translation({-3, 0, 0})));
        r.submit_skinned(mesh, mat, math::Mat4::identity(), straight, box);
        r.submit_skinned(mesh, mat, math::Mat4::translation({3, 0, 0}), right, box.transformed(math::Mat4::translation({3, 0, 0})));
    };
    const Shot on = shoot(*sr.value(), content, true, "skinning_batch", {0, 1, 10});
    const Shot off = shoot(*sr.value(), content, false, nullptr, {0, 1, 10});
    CHECK(on.stats.skinned_draws == 3);
    CHECK(on.stats.visible == 3);
    CHECK(on.stats.main_batches == 1);
    CHECK(off.stats.main_batches == 3);
    CHECK(max_diff(on.px, off.px) <= 1);
    CHECK(on.red({-3.7f, 1.0f, 0.25f})); // left column bent to -X
    CHECK(on.red({3.7f, 1.0f, 0.25f}));  // right column bent to +X
    CHECK(on.red({0, 1.75f, 0.25f}));    // middle column straight
    CHECK_FALSE(on.red({-3, 1.75f, 0.25f}));
    ctx.destroy(mesh);
}

TEST_CASE("gpu skinning: animator-driven model renders posed through extract_scene") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    animation::register_components();
    runtime::GpuAssetCache cache(ctx, nullptr);
    const Uuid id = Uuid::generate();
    const assets::CookedModel cooked = test::make_skinned_column(true);
    REQUIRE(cache.add_model(id, cooked).ok());
    const runtime::GpuModel* gm = cache.model(id).value();
    CHECK(gm->max_joint[0] == 1);
    REQUIRE(gm->joint_bounds[0].size() == 2);
    CHECK(gm->joint_bounds[0][1].min.y == doctest::Approx(1.0f));

    scene::Scene scene;
    const scene::Entity root = runtime::instantiate_model(scene, id, cooked, "Column Model");
    const auto* animator = scene.world().get<animation::AnimatorComponent>(root);
    REQUIRE(animator);
    CHECK(animator->clip == "Bend");
    const scene::Entity body = scene.find_by_name("Column");
    REQUIRE(scene.world().get<animation::SkinnedMeshComponent>(body));
    CHECK(scene.world().get<animation::SkinnedMeshComponent>(body)->skeleton == scene.uuid(root));

    animation::AnimationSystem anim;
    anim.add_set(id, std::make_shared<animation::AnimationSet>(assets::make_animation_set(cooked)));
    scene.world().get<animation::AnimatorComponent>(root)->loop = false;
    anim.update(scene, 2.0f); // clamps at the end of Bend: 90 degrees
    scene.update_transforms();

    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    runtime::ExtractStats stats;
    const Shot bent = shoot(*sr.value(), [&](SceneRenderer& r) {
        runtime::SceneViewSettings sv;
        const Camera3D cam = Camera3D::look_at({0, 1, 8}, {0, 1, 0});
        sv.environment.draw_sky = false;
        sv.environment.clear_color = {0, 0, 0, 1};
        stats = runtime::extract_scene(scene, cache, r, sv, &cam);
    }, true, "skinning_runtime");
    CHECK(stats.skinned == 1);
    CHECK(bent.stats.skinned_draws == 1);
    CHECK(bent.red({-0.7f, 1.0f, 0.25f}));
    CHECK_FALSE(bent.red({0, 1.75f, 0.25f}));

    // Moving the whole character moves the skinned mesh with it (palette is mesh-relative).
    scene.world().get<scene::Transform>(root)->position = {1.5f, 0, 0};
    scene.update_transforms();
    const Shot moved = shoot(*sr.value(), [&](SceneRenderer& r) {
        const Camera3D cam = Camera3D::look_at({0, 1, 8}, {0, 1, 0});
        runtime::SceneViewSettings sv;
        sv.environment.draw_sky = false;
        sv.environment.clear_color = {0, 0, 0, 1};
        runtime::extract_scene(scene, cache, r, sv, &cam);
    });
    CHECK(moved.red({0.8f, 1.0f, 0.25f}));
    CHECK(moved.red({1.5f, 0.5f, 0.25f}));

    // Deleting the skeleton's joints falls back to the bind pose without crashing.
    scene.destroy(scene.find_by_name("Hip"));
    scene.update_transforms();
    const Shot orphan = shoot(*sr.value(), [&](SceneRenderer& r) {
        const Camera3D cam = Camera3D::look_at({0, 1, 8}, {0, 1, 0});
        runtime::SceneViewSettings sv;
        sv.environment.draw_sky = false;
        runtime::extract_scene(scene, cache, r, sv, &cam);
    });
    CHECK(orphan.red({1.5f, 1.75f, 0.25f}));
}

TEST_CASE("gpu skinning: crowd agents draw baked clips like animator-driven models (ADR-0200)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    animation::register_components();
    runtime::GpuAssetCache cache(ctx, nullptr);
    const Uuid id = Uuid::generate();
    const assets::CookedModel cooked = test::make_skinned_column(true);
    REQUIRE(cache.add_model(id, cooked).ok());
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    const auto view = [&](scene::Scene& scene, runtime::ExtractStats* stats, const char* save = nullptr) {
        return shoot(*sr.value(), [&](SceneRenderer& r) {
            runtime::SceneViewSettings sv;
            const Camera3D cam = Camera3D::look_at({0, 1, 8}, {0, 1, 0});
            sv.environment.draw_sky = false;
            sv.environment.clear_color = {0, 0, 0, 1};
            const runtime::ExtractStats st = runtime::extract_scene(scene, cache, r, sv, &cam);
            if (stats) *stats = st;
        }, true, save);
    };

    // Reference: the regular path, Bend clamped at its end (90 degrees).
    scene::Scene ref;
    const scene::Entity root = runtime::instantiate_model(ref, id, cooked, "Column Model");
    animation::AnimationSystem anim;
    anim.add_set(id, std::make_shared<animation::AnimationSet>(assets::make_animation_set(cooked)));
    ref.world().get<animation::AnimatorComponent>(root)->loop = false;
    anim.update(ref, 2.0f);
    ref.update_transforms();
    const Shot animator = view(ref, nullptr);

    // The same pose from a single crowd agent: one entity, no joints, no MeshRenderer.
    scene::Scene crowd;
    const scene::Entity agent = crowd.create("Agent");
    {
        auto& ca = crowd.world().add<animation::CrowdAgentComponent>(agent);
        ca.model = id;
        ca.clip = "Bend";
        ca.loop = false;
        ca.time = 2.0f;
    }
    crowd.update_transforms(); // adds WorldTransform: component references are re-fetched after it
    const auto agent_of = [&]() -> animation::CrowdAgentComponent& { return *crowd.world().get<animation::CrowdAgentComponent>(agent); };
    runtime::ExtractStats stats;
    const Shot baked = view(crowd, &stats);
    test::save_output("crowd_agent", W, H, baked.px);
    CHECK(stats.crowd_agents == 1);
    CHECK(stats.skinned == 1);
    CHECK(baked.stats.skinned_draws == 1);
    CHECK(baked.red({-0.7f, 1.0f, 0.25f}));
    CHECK_FALSE(baked.red({0, 1.75f, 0.25f}));
    CHECK(max_diff(animator.px, baked.px) <= 8);
    CHECK(cache.crowd_clip_count() == 1);

    // Mid-clip time on the looping clip, and the bind pose for an unknown clip.
    agent_of().loop = true;
    agent_of().time = 1.0f; // the end of a loop is its start: upright
    CHECK(view(crowd, nullptr, nullptr).red({0, 1.75f, 0.25f}));
    agent_of().clip = "Missing";
    runtime::ExtractStats missing;
    const Shot rest = view(crowd, &missing);
    CHECK(missing.crowd_agents == 1);
    CHECK(rest.red({0, 1.75f, 0.25f}));

    // Many agents of one clip share one baked buffer and merge into one instanced batch per pass.
    for (int i = 0; i < 63; ++i) {
        const scene::Entity e = crowd.create("Agent");
        crowd.world().add<animation::CrowdAgentComponent>(e, animation::CrowdAgentComponent{.model = id, .clip = "Bend", .time = 0.03f * static_cast<float>(i)});
        crowd.world().get<scene::Transform>(e)->position = {-3.0f + 0.1f * static_cast<float>(i), 0, -2.0f};
    }
    crowd.update_transforms();
    const Shot many = view(crowd, &stats);
    CHECK(stats.crowd_agents == 64);
    CHECK(many.stats.skinned_draws == 64);
    CHECK(many.stats.main_batches <= 2); // the 63 "Bend" agents, and agent 0 on its bind-pose bake
    CHECK(cache.crowd_clip_count() == 2); // "Bend" and the bind-pose bake of "Missing"

    // Evicting the model releases its bakes; the next frame bakes again.
    cache.evict(id);
    CHECK(cache.crowd_clip_count() == 0);
    REQUIRE(cache.add_model(id, cooked).ok());
    view(crowd, &stats);
    CHECK(stats.crowd_agents == 64);
    CHECK(cache.crowd_clip_count() == 2);
}

TEST_CASE("gpu skinning: crowd agents cost less CPU per frame than animator characters (ADR-0200)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    animation::register_components();
    runtime::GpuAssetCache cache(ctx, nullptr);
    const Uuid id = Uuid::generate();
    const assets::CookedModel cooked = test::make_skinned_column(true);
    REQUIRE(cache.add_model(id, cooked).ok());
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    constexpr int N = 1000;
    const auto place = [](int i) { return math::Vec3{static_cast<float>(i % 40) * 0.6f - 12.0f, 0, -static_cast<float>(i / 40) * 0.6f}; };
    animation::AnimationSystem anim;
    anim.add_set(id, std::make_shared<animation::AnimationSet>(assets::make_animation_set(cooked)));
    scene::Scene chars, crowd;
    for (int i = 0; i < N; ++i) {
        const scene::Entity r = runtime::instantiate_model(chars, id, cooked, "Column Model");
        chars.world().get<scene::Transform>(r)->position = place(i);
        chars.world().get<animation::AnimatorComponent>(r)->time = 0.01f * static_cast<float>(i);
        const scene::Entity a = crowd.create("Agent");
        crowd.world().add<animation::CrowdAgentComponent>(a, animation::CrowdAgentComponent{.model = id, .clip = "Bend", .time = 0.01f * static_cast<float>(i)});
        crowd.world().get<scene::Transform>(a)->position = place(i);
    }
    // CPU time of animation + transforms + extraction (the renderer's GPU work is not timed).
    const auto frame_ms = [&](scene::Scene& scene, runtime::ExtractStats& stats) {
        double best = 1e9;
        for (int f = 0; f < 6; ++f) {
            const auto t0 = std::chrono::steady_clock::now();
            anim.update(scene, 1.0f / 60.0f);
            scene.update_transforms();
            sr.value()->begin(Camera3D::look_at({0, 6, 14}, {0, 0, -8}), DirectionalLight{}, Environment{});
            runtime::SceneViewSettings sv;
            const Camera3D cam = Camera3D::look_at({0, 6, 14}, {0, 0, -8});
            stats = runtime::extract_scene(scene, cache, *sr.value(), sv, &cam);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (f > 0) best = std::min(best, ms); // frame 0 binds skeletons / bakes the clip
        }
        return best;
    };
    runtime::ExtractStats a, c;
    const double animator_ms = frame_ms(chars, a);
    const double crowd_ms = frame_ms(crowd, c);
    CHECK(a.skinned == N);
    CHECK(c.crowd_agents == N);
    MESSAGE(N << " skinned characters, CPU animation+extract: Animator " << animator_ms << " ms, CrowdAgent " << crowd_ms << " ms");
    CHECK(crowd_ms < animator_ms);
}

// ADR-0205: dual quaternion skinning on the GPU. A tube (elbow/forearm stand-in) is twisted 170
// degrees about its axis and drawn side-on; linear blending pinches the middle to a thread while
// dual quaternions keep its width. The DQS draw must also match the CPU reference
// (render::skin_vertices) drawn as a static mesh, so shader and CPU fallback agree.
TEST_CASE("gpu skinning: dual quaternion palettes keep a twisted limb's width in the runtime renderer") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    render::MeshData tube = test::make_tube();
    auto mesh = ctx.create_mesh(tube, "dq_tube").value();
    REQUIRE(mesh.skinned());
    const Material mat = column_material();
    const math::Mat4 place = math::Mat4::translation({-1, 1, 0});
    const math::Mat4 twist = test::joint1(math::Quat::axis_angle({1, 0, 0}, math::radians(170.0f)));
    std::vector<math::Mat4> lbs = {math::Mat4::identity(), twist};
    std::vector<math::Mat4> dqs = lbs;
    render::encode_dq_palette(dqs);
    const math::Aabb bounds = math::Aabb{{-0.3f, -0.3f, -0.3f}, {2.3f, 0.3f, 0.3f}}.transformed(place);
    const math::Vec3 eye{0, 1, 3};
    const Shot shot_l = shoot(*sr.value(), [&](SceneRenderer& r) { r.submit_skinned(mesh, mat, place, lbs, bounds); }, true,
                              "skinning_twist_lbs", eye);
    const Shot shot_d = shoot(*sr.value(), [&](SceneRenderer& r) { r.submit_skinned(mesh, mat, place, dqs, bounds); }, true,
                              "skinning_twist_dqs", eye);
    // CPU reference of the same DQS pose as a plain mesh.
    render::MeshData cpu = tube;
    render::skin_vertices(tube, lbs, render::SkinningMethod::DualQuaternion, cpu.vertices);
    cpu.skin.clear();
    auto cpu_mesh = ctx.create_mesh(cpu, "dq_tube_cpu").value();
    const Shot shot_c = shoot(*sr.value(), [&](SceneRenderer& r) { r.submit(cpu_mesh, mat, place); }, true, "skinning_twist_cpu", eye);

    // Covered pixels in the image column through the tube's middle (world x = 0).
    const auto column_height = [&](const Shot& s) {
        const math::Vec4 clip = s.cam.projection(float(W) / float(H)) * s.cam.view * math::Vec4(math::Vec3{0, 1, 0}, 1.0f);
        const auto x = static_cast<std::uint32_t>((clip.x / clip.w * 0.5f + 0.5f) * float(W));
        int n = 0;
        for (std::uint32_t y = 0; y < H; ++y) {
            const auto c = test::pixel(s.px, W, x, y);
            if (channel(c, 0) > 80 && channel(c, 0) > channel(c, 1) + 50) ++n;
        }
        return n;
    };
    const int h_l = column_height(shot_l), h_d = column_height(shot_d), h_c = column_height(shot_c);
    MESSAGE("twisted tube middle column: LBS " << h_l << " px, DQS " << h_d << " px, CPU DQS " << h_c << " px");
    CHECK(shot_d.stats.skinned_draws == 1);
    CHECK(h_d >= 3 * std::max(h_l, 1));          // LBS pinches the middle, DQS does not
    CHECK(std::abs(h_d - h_c) <= 1);             // GPU DQS == CPU reference
    CHECK(max_diff(shot_d.px, shot_c.px) <= 24); // whole image (shading on the pinch-free tube)
    // The tube's ends are rigid: both methods agree there.
    CHECK(shot_l.red({-0.8f, 1.0f, 0.2f}));
    CHECK(shot_d.red({-0.8f, 1.0f, 0.2f}));
    ctx.destroy(cpu_mesh);
    ctx.destroy(mesh);
}
