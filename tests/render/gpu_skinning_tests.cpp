// SPDX-License-Identifier: Apache-2.0
// GPU skinning (Slice 12): skinned meshes follow their joint palettes in the main, depth
// pre-pass and shadow passes, skinned instances batch, and the runtime path (cooked model ->
// instantiate -> Animator -> extract_scene) poses the mesh from animated entities.
#include "../assets/skinned_model.hpp"
#include "gpu_fixture.hpp"
#include "victus/runtime/scene_rendering.hpp"

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
