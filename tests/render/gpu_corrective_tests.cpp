// SPDX-License-Identifier: Apache-2.0
// Corrective shapes in the runtime renderer (ADR-0220): Orion with a baked forearm-twist
// corrective, drawn through extract_scene with linear blend skinning, must look like dual
// quaternion skinning at the twisted pose and like the plain model at rest. The driver weight is
// computed from the scene's joint entities, so this is the path clips, IK and Studio poses take.
#include "gpu_fixture.hpp"
#include "victus/animation/animation.hpp"
#include "victus/assets/correctives.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/instantiate.hpp"
#include "victus/project/characters.hpp"
#include "victus/runtime/scene_rendering.hpp"

using namespace victus;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 256, H = 192;
constexpr float kDeg = 3.14159265f / 180.0f;

assets::CookedModel orion() {
    const project::StarterCharacter* c = project::find_starter_character("orion");
    REQUIRE(c);
    assets::ModelImportSettings s;
    s.import_textures = false;
    s.lods = 0;
    auto m = assets::import_gltf_memory(project::starter_character_glb(*c), c->model_path, s);
    REQUIRE(m.ok());
    return std::move(m.value());
}

struct Frame {
    std::vector<std::uint8_t> px;
    runtime::ExtractStats stats;
};

// Orion instantiated from `model`, its right forearm twisted `degrees` about its bone axis, drawn
// with `method`.
Frame draw(runtime::GpuAssetCache& cache, const Uuid& id, const assets::CookedModel& model, float degrees,
           animation::SkinningMode method, const char* save) {
    scene::Scene s;
    const scene::Entity root = assets::instantiate_model(s, id, model, "Orion");
    s.world().remove<animation::AnimatorComponent>(root);
    s.world().each<animation::SkinnedMeshComponent>([&](ecs::Entity, animation::SkinnedMeshComponent& sm) { sm.method = method; });
    const scene::Entity arm = s.find_by_name("LowerArm.R");
    REQUIRE_FALSE(arm.is_null());
    auto* t = s.world().get<scene::Transform>(arm);
    t->rotation = math::normalize(t->rotation * math::Quat::axis_angle({0, 1, 0}, degrees * kDeg));
    s.update_transforms();
    const math::Vec3 at = s.world_matrix(arm).transform_point({0, 0.12f, 0});
    const render::Camera3D cam = render::Camera3D::look_at(at + math::Vec3{0.1f, 0.15f, 0.75f}, at);
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    runtime::SceneViewSettings sv;
    sv.environment.draw_sky = false;
    sv.environment.clear_color = {0, 0, 0, 1};
    const scene::Entity sun = s.create("Sun");
    s.world().add<scene::LightComponent>(sun).intensity = 3.0f;
    s.world().get<scene::Transform>(sun)->rotation = math::Quat::axis_angle({1, 0, 0}, -0.9f);
    s.update_transforms();
    Frame f;
    f.stats = runtime::extract_scene(s, cache, **sr, sv, &cam);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    f.px = dev.read_texture(target).value();
    dev.destroy(target);
    if (save) test::save_output(save, W, H, f.px);
    return f;
}

// Pixels whose colour differs by more than 40/255 in any channel, and pixels covered in `a`.
std::pair<std::size_t, std::size_t> diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t n = 0, covered = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
        int d = 0;
        for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(a[i + k]) - int(b[i + k])));
        n += d > 40;
        covered += a[i] + a[i + 1] + a[i + 2] > 6;
    }
    return {n, covered};
}

} // namespace

TEST_CASE("gpu correctives: a baked forearm-twist corrective makes runtime LBS look like DQS when twisted and changes nothing at rest") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    animation::register_components();
    const assets::CookedModel plain = orion();
    assets::CookedModel fixed = plain;
    assets::CorrectiveSettings cs;
    cs.joint = "LowerArm.R";
    cs.axis = {0, 1, 0};
    cs.degrees = 150;
    REQUIRE(assets::add_corrective(fixed, cs).ok());
    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    const Uuid plain_id = Uuid::generate(), fixed_id = Uuid::generate();
    REQUIRE(cache.add_model(plain_id, plain).ok());
    REQUIRE(cache.add_model(fixed_id, fixed).ok());

    using animation::SkinningMode;
    const Frame lbs = draw(cache, plain_id, plain, 150, SkinningMode::Linear, "corrective_twist_lbs");
    const Frame dqs = draw(cache, plain_id, plain, 150, SkinningMode::DualQuaternion, "corrective_twist_dqs");
    const Frame cor = draw(cache, fixed_id, fixed, 150, SkinningMode::Linear, "corrective_twist_lbs_corrective");
    const auto [cor_vs_dqs, covered] = diff(cor.px, dqs.px);
    const auto [lbs_vs_dqs, covered_lbs] = diff(lbs.px, dqs.px);
    MESSAGE("forearm twisted 150 degrees, " << covered << " px covered: LBS+corrective vs DQS " << cor_vs_dqs << " px, plain LBS vs DQS "
                                            << lbs_vs_dqs << " px; meshes with correctives " << cor.stats.correctives << ", morphed "
                                            << cor.stats.morphed);
    CHECK(covered > 3000);
    CHECK(cor.stats.correctives >= 1);
    CHECK(cor.stats.morphed >= 1);
    CHECK(lbs.stats.correctives == 0);
    CHECK(lbs_vs_dqs > 100);              // the candy wrapper is visible
    CHECK(cor_vs_dqs * 10 < lbs_vs_dqs);  // and the corrective removes it

    // At rest the drivers weigh 0: no corrective, no morphed copy, the same image as the plain model.
    const Frame rest_plain = draw(cache, plain_id, plain, 0, SkinningMode::Linear, nullptr);
    const Frame rest_fixed = draw(cache, fixed_id, fixed, 0, SkinningMode::Linear, "corrective_rest");
    CHECK(rest_fixed.stats.correctives == 0);
    CHECK(diff(rest_fixed.px, rest_plain.px).first == 0);
}
