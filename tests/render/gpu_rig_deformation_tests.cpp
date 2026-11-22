// SPDX-License-Identifier: Apache-2.0
// Deformation verified in the runtime renderer (ADR-0207): the reference quadruped and mech
// rigs, Orion under extreme poses and the complex humanoid (Orion plus 19 extension bones) are
// drawn GPU-skinned (linear and dual quaternion palettes, lit, with shadows) and compared with
// the same pose skinned on the CPU and drawn as a static mesh. Importers accepting a skeleton
// prove nothing; these images are what a player sees.
#include "gpu_fixture.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/reference_rigs.hpp"
#include "victus/assets/skeleton.hpp"
#include "victus/project/characters.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/skinning.hpp"

#include <chrono>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 256, H = 192;

struct Draw {
    GpuMesh mesh;
    Material mat;
    std::vector<math::Mat4> palette; // empty: static
    math::Aabb bounds;
};

std::vector<std::uint8_t> shoot(SceneRenderer& r, const std::vector<Draw>& draws, math::Vec3 eye, math::Vec3 at,
                                const char* save) {
    auto& dev = gpu().ctx->device();
    auto target = test::make_target(W, H);
    RenderSettings s = RenderSettings::preset(QualityTier::High);
    s.taa = false;
    r.set_settings(s);
    DirectionalLight sun;
    sun.direction = math::normalize(math::Vec3{-0.4f, -1.0f, -0.6f});
    sun.shadow_distance = 10;
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    r.begin(Camera3D::look_at(eye, at), sun, env);
    for (const Draw& d : draws) {
        if (d.palette.empty()) r.submit(d.mesh, d.mat, math::Mat4::identity());
        else r.submit_skinned(d.mesh, d.mat, math::Mat4::identity(), d.palette, d.bounds);
    }
    auto cmd = dev.begin_commands();
    REQUIRE(r.render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target).value();
    dev.destroy(target);
    if (save) test::save_output(save, W, H, px);
    return px;
}

struct Compare {
    std::size_t covered_a{0}, covered_b{0}, mismatched{0};
    double mismatch_fraction() const { return double(mismatched) / double(std::max<std::size_t>(1, std::max(covered_a, covered_b))); }
};
// Pixels covered (non-black) in either image and pixels whose colour differs by more than 40/255
// in any channel (an edge pixel shifting by one, or a wrong surface).
Compare compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    Compare c;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
        const bool ca = a[i] + a[i + 1] + a[i + 2] > 6, cb = b[i] + b[i + 1] + b[i + 2] > 6;
        c.covered_a += ca;
        c.covered_b += cb;
        int d = 0;
        for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(a[i + k]) - int(b[i + k])));
        c.mismatched += d > 40;
    }
    return c;
}

math::Aabb bounds_of(const std::vector<Vertex3D>& v) {
    math::Aabb b{v[0].position, v[0].position};
    for (const auto& x : v) b.expand(x.position);
    return b;
}

struct RigResult {
    Compare gpu_vs_cpu_lbs, gpu_vs_cpu_dqs, lbs_vs_dqs;
};

// Renders every skinned mesh of `m` posed by `world`: GPU LBS, GPU DQS, CPU LBS and CPU DQS.
RigResult verify(const assets::CookedModel& m, const std::vector<math::Mat4>& world, math::Vec3 eye, math::Vec3 at, const char* name) {
    auto& ctx = *gpu().ctx;
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    const auto lbs = assets::pose_palette(m, world);
    auto dqs = lbs;
    encode_dq_palette(dqs);
    std::vector<GpuMesh> owned;
    std::vector<Draw> gl, gd, cl, cd;
    for (const auto& mesh : m.meshes) {
        if (!mesh.data.skinned()) continue;
        Material mat;
        const auto& mm = m.materials[std::min<std::size_t>(mesh.material, m.materials.size() - 1)];
        mat.base_color = {mm.base_color.r, mm.base_color.g, mm.base_color.b, 1};
        mat.metallic = std::min(mm.metallic, 0.3f);
        mat.roughness = std::max(mm.roughness, 0.4f);
        auto gpu_mesh = ctx.create_mesh(mesh.data, "rig_gpu").value();
        owned.push_back(gpu_mesh);
        for (const auto method : {SkinningMethod::Linear, SkinningMethod::DualQuaternion}) {
            MeshData cpu = mesh.data;
            skin_vertices(mesh.data, lbs, method, cpu.vertices);
            cpu.skin.clear();
            math::Aabb b = bounds_of(cpu.vertices);
            b.min = b.min - math::Vec3{0.05f, 0.05f, 0.05f};
            b.max = b.max + math::Vec3{0.05f, 0.05f, 0.05f};
            auto cpu_mesh = ctx.create_mesh(cpu, "rig_cpu").value();
            owned.push_back(cpu_mesh);
            const bool dq = method == SkinningMethod::DualQuaternion;
            (dq ? gd : gl).push_back({gpu_mesh, mat, dq ? dqs : lbs, b});
            (dq ? cd : cl).push_back({cpu_mesh, mat, {}, b});
        }
    }
    const std::string n(name);
    const auto img_gl = shoot(*sr.value(), gl, eye, at, (n + "_gpu_lbs").c_str());
    const auto img_gd = shoot(*sr.value(), gd, eye, at, (n + "_gpu_dqs").c_str());
    const auto img_cl = shoot(*sr.value(), cl, eye, at, (n + "_cpu_lbs").c_str());
    const auto img_cd = shoot(*sr.value(), cd, eye, at, (n + "_cpu_dqs").c_str());
    for (auto h : owned) ctx.destroy(h);
    RigResult r{compare(img_gl, img_cl), compare(img_gd, img_cd), compare(img_gl, img_gd)};
    MESSAGE(n << ": covered " << r.gpu_vs_cpu_dqs.covered_a << " px; GPU vs CPU mismatch LBS " << r.gpu_vs_cpu_lbs.mismatched
                 << " px (" << r.gpu_vs_cpu_lbs.mismatch_fraction() * 100 << "%), DQS " << r.gpu_vs_cpu_dqs.mismatched << " px ("
                 << r.gpu_vs_cpu_dqs.mismatch_fraction() * 100 << "%); LBS vs DQS differ by " << r.lbs_vs_dqs.mismatched << " px");
    CHECK(r.gpu_vs_cpu_lbs.covered_a > 1500);
    CHECK(r.gpu_vs_cpu_lbs.mismatch_fraction() < 0.02);
    CHECK(r.gpu_vs_cpu_dqs.mismatch_fraction() < 0.02);
    return r;
}

std::uint32_t node_of(const assets::CookedModel& m, std::string_view name) {
    for (std::size_t i = 0; i < m.nodes.size(); ++i)
        if (m.nodes[i].name == name) return static_cast<std::uint32_t>(i);
    FAIL("missing node " << name);
    return 0;
}

const assets::CookedModel& orion() {
    static assets::CookedModel model = [] {
        const project::StarterCharacter* c = project::find_starter_character("orion");
        REQUIRE(c);
        assets::ModelImportSettings s;
        s.import_textures = false;
        s.lods = 0;
        auto m = assets::import_gltf_memory(project::starter_character_glb(*c), c->model_path, s);
        REQUIRE(m.ok());
        return std::move(m.value());
    }();
    return model;
}

} // namespace

TEST_CASE("gpu rig deformation: the quadruped creature matches the CPU reference in clips and extreme poses") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const auto m = assets::make_reference_rig(assets::ReferenceRig::Quadruped);
    const math::Vec3 eye{1.0f, 0.75f, 0.95f}, at{0, 0.45f, 0};
    verify(m, assets::pose_world(m, &m.animations[0], 0.3f), eye, at, "rig_quadruped_walk");
    verify(m, assets::pose_world(m, &m.animations[1], 1.0f), {1.5f, 1.0f, 1.3f}, {0, 0.55f, 0}, "rig_quadruped_sit");
    std::vector<std::pair<std::uint32_t, math::Quat>> pose;
    for (int i = 1; i <= 7; ++i) pose.emplace_back(node_of(m, "Tail" + std::to_string(i)), math::Quat::axis_angle({0, 0, 1}, 2.1f));
    pose.emplace_back(node_of(m, "Neck2"), math::Quat::axis_angle({0, 1, 0}, 0.75f) * math::Quat::axis_angle({0, 0, 1}, 1.2f));
    pose.emplace_back(node_of(m, "FrontLowerLeg.R"), math::Quat::axis_angle({0, 1, 0}, 2.4f));
    const RigResult r = verify(m, assets::pose_world(m, nullptr, 0, pose), {-0.9f, 0.9f, -1.1f}, {0, 0.5f, -0.2f}, "rig_quadruped_extreme");
    CHECK(r.lbs_vs_dqs.mismatched > 0); // the twisted tail renders differently: the method reaches the GPU
}

TEST_CASE("gpu rig deformation: the mech stays rigid and matches the CPU reference") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const auto m = assets::make_reference_rig(assets::ReferenceRig::Mech);
    const RigResult r = verify(m, assets::pose_world(m, &m.animations[0], 0.7f), {2.0f, 1.5f, 2.4f}, {0, 0.75f, 0.4f}, "rig_mech_work");
    // Rigid parts: LBS and DQS give the same image (single-influence vertices).
    CHECK(r.lbs_vs_dqs.mismatch_fraction() < 0.01);
}

TEST_CASE("gpu rig deformation: Orion under extreme poses and the complex humanoid match the CPU reference") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const assets::CookedModel& o = orion();
    const std::pair<std::uint32_t, math::Quat> pose[4] = {
        {node_of(o, "LowerArm.R"), math::Quat::axis_angle({0, 1, 0}, 2.6f) * math::Quat::axis_angle({1, 0, 0}, 2.4f)},
        {node_of(o, "Hand.R"), math::Quat::axis_angle({0, 1, 0}, 1.4f)},
        {node_of(o, "LowerLeg.L"), math::Quat::axis_angle({1, 0, 0}, 2.4f)},
        {node_of(o, "UpperArm.L"), math::Quat::axis_angle({0, 1, 0}, 2.6f)},
    };
    const math::Vec3 eye{0.0f, 1.0f, 2.3f}, at{0, 0.9f, 0};
    verify(o, assets::pose_world(o, nullptr, 0, pose), eye, at, "rig_orion_extreme");

    // Complex humanoid: the extension's new joints posed (roll joints twisted, jaw/tongue/lip and
    // antenna bent) on top of an animated clip.
    assets::CookedModel ext = o;
    assets::SkeletonEditOptions opt;
    for (const char* n : project::character_core_joint_names()) opt.protected_bones.emplace_back(n);
    const auto edits = assets::complex_humanoid_extension();
    REQUIRE(assets::apply_skeleton_edits(ext, edits, opt).ok());
    const animation::Clip* walk = nullptr;
    for (const auto& c : ext.animations)
        if (c.name == "Walk") walk = &c;
    const std::pair<std::uint32_t, math::Quat> extra[5] = {
        {node_of(ext, "ForeArmRoll.L"), math::Quat::axis_angle({0, 1, 0}, 1.4f)},
        {node_of(ext, "ForeArmRoll.R"), math::Quat::axis_angle({0, 1, 0}, -1.4f)},
        {node_of(ext, "Dyn.Antenna1"), math::Quat::axis_angle({0, 0, 1}, 0.8f)},
        {node_of(ext, "Dyn.Antenna2"), math::Quat::axis_angle({0, 0, 1}, 0.8f)},
        {node_of(ext, "Lip.Corner.L"), math::Quat::axis_angle({1, 0, 0}, 0.4f)},
    };
    const auto t0 = std::chrono::steady_clock::now();
    const auto world = assets::pose_world(ext, walk, 0.4f, extra);
    const double pose_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("complex humanoid: " << ext.skins[0].joints.size() << " joints, pose " << pose_ms << " ms, palette "
                                 << ext.skins[0].joints.size() * sizeof(math::Mat4) << " B");
    verify(ext, world, eye, at, "rig_complex_humanoid");
}
