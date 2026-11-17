// SPDX-License-Identifier: Apache-2.0
// Project surface shaders (.vshader) end to end: a material referencing a shader asset is
// compiled at load time and changes what reaches the screen, per-material parameters and time
// reach the shader, broken shaders fall back to built-in PBR, and hot reload recompiles them.
#include "gpu_fixture.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/surface_shader.hpp"

#include <chrono>
#include <filesystem>

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;
namespace sfs = std::filesystem;

namespace {

struct ShaderScene {
    fs::Path project;
    std::unique_ptr<assets::AssetDatabase> db;
    Uuid shader, material;
    Scene scene;
    Entity plane;
    int bump{0};

    ShaderScene() {
        project = sfs::temp_directory_path() / ("victus_shader_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(project / "assets" / "shaders");
        auto opened = assets::AssetDatabase::open(project);
        REQUIRE(opened.ok());
        db = std::move(opened).value();
        write_shader("void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = i.params[0].rgb;\n}\n");
        assets::MaterialAsset m;
        m.unlit = true; // exact colours on screen, independent of lighting
        m.base_color = {1, 1, 1, 1};
        m.shader = shader;
        m.shader_params[0] = {0, 0, 1, 0};
        REQUIRE(assets::save_material_file(project / "assets" / "glow.vmat", m).ok());
        (void)db->refresh();
        material = db->find_path("glow.vmat")->uuid;

        const Entity cam = scene.create("Camera");
        scene.world().get<Transform>(cam)->position = {0, 6, 0};
        scene.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
        scene.world().add<CameraComponent>(cam);
        plane = scene.create("Floor");
        scene.world().get<Transform>(plane)->scale = {20, 1, 20};
        auto& mr = scene.world().add<MeshRendererComponent>(plane);
        mr.primitive = "plane";
        mr.material_asset = material;
        scene.update_transforms();
    }
    ~ShaderScene() {
        db.reset();
        std::error_code ec;
        sfs::remove_all(project, ec);
    }
    void write_shader(const std::string& code) {
        const fs::Path file = project / "assets" / "shaders" / "tint.vshader";
        REQUIRE(fs::write_atomic(file, std::span(reinterpret_cast<const std::uint8_t*>(code.data()), code.size())).ok());
        sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(++bump * 5));
        (void)db->refresh();
        shader = db->find_path("shaders/tint.vshader")->uuid;
    }
};

// Centre pixel of the rendered scene (RGBA8).
std::uint32_t centre(ShaderScene& s, runtime::GpuAssetCache& cache, std::optional<double> time = {}) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    if (time) (*sr)->set_time(*time);
    constexpr std::uint32_t W = 96, H = 64;
    auto target = test::make_target(W, H);
    const auto stats = runtime::extract_scene(s.scene, cache, **sr, {});
    CHECK(stats.meshes == 1);
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("surface_shader", W, H, *px);
    dev.destroy(target);
    return pixel(*px, W, W / 2, H / 2);
}
bool is_blue(std::uint32_t p) { return channel(p, 2) > 150 && channel(p, 0) < 60 && channel(p, 1) < 60; }
bool is_white(std::uint32_t p) { return channel(p, 0) > 150 && channel(p, 1) > 150 && channel(p, 2) > 150; }

} // namespace

TEST_CASE("surface shader assets: material references, JSON and validation") {
    CHECK(assets::kind_for_extension(".VShader") == assets::AssetKind::Shader);
    CHECK(std::string_view(assets::to_string(assets::AssetKind::Shader)) == "shader");
    assets::MaterialAsset m;
    m.shader = Uuid::generate();
    m.shader_params[0] = {1, 2, 3, 4};
    m.shader_params[2] = {0.5f, 0, 0, 0};
    auto back = assets::MaterialAsset::from_json(m.to_json());
    REQUIRE(back.ok());
    CHECK(*back == m);
    CHECK(m.to_json()["shader_params"].size() == 3); // trailing zero vectors are not written
    auto shortp = json::parse(R"({"victus_material": 1, "shader_params": [[2], [1, 2]]})");
    REQUIRE(shortp.ok());
    auto sp = assets::MaterialAsset::from_json(*shortp);
    REQUIRE(sp.ok());
    CHECK(sp->shader_params[0] == math::Vec4{2, 0, 0, 0});
    CHECK(sp->shader_params[1] == math::Vec4{1, 2, 0, 0});
    for (const char* bad : {R"({"victus_material": 1, "shader": 5})", R"({"victus_material": 1, "shader_params": [[1],[1],[1],[1],[1]]})",
                            R"({"victus_material": 1, "shader_params": [["x"]]})", R"({"victus_material": 1, "shader_params": [[1,2,3,4,5]]})"}) {
        auto doc = json::parse(bad);
        REQUIRE(doc.ok());
        CHECK_FALSE(assets::MaterialAsset::from_json(*doc).ok());
    }

    // The editor template compiles; extra resources are rejected with a clear message.
    CHECK(runtime::compile_surface_shader(runtime::surface_shader_template(), "template.vshader").ok());
    auto extra = runtime::compile_surface_shader("layout(set = 2, binding = 6) uniform sampler2D u_extra;\n"
                                                 "void surface(SurfaceInput i, inout Surface s) { s.base_color = texture(u_extra, i.uv).rgb; }\n",
                                                 "extra.vshader");
    REQUIRE_FALSE(extra.ok());
    CHECK(extra.error().message.find("extra.vshader") != std::string::npos);
    auto broken = runtime::compile_surface_shader("void surface(SurfaceInput i, inout Surface s) {\n  s.nope = 1.0;\n}\n", "b.vshader");
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message.find("b.vshader:2") != std::string::npos);
}

TEST_CASE("gpu: surface shaders recolour meshes, read params and time, fall back and hot reload") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    ShaderScene s;
    runtime::GpuAssetCache cache(*gpu().ctx, s.db.get());

    // params[0] = blue replaces the white base colour.
    auto mat = cache.material(s.material);
    REQUIRE(mat.ok());
    CHECK(mat->surface_shader);
    CHECK(mat->shader_params[0] == math::Vec4{0, 0, 1, 0});
    CHECK(is_blue(centre(s, cache)));

    // Time reaches the shader: brightness follows fract(time / 10).
    runtime::AssetHotReloader reloader(*s.db);
    s.write_shader("void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = vec3(fract(i.time / 10.0));\n}\n");
    auto changes = reloader.check();
    REQUIRE(changes.shaders.size() == 1);
    CHECK(changes.shaders[0] == "shaders/tint.vshader");
    CHECK(changes.restart_needed.empty());
    runtime::AssetHotReloader::apply(changes, &cache, nullptr);
    const auto dark = centre(s, cache, 1.0);
    const auto bright = centre(s, cache, 9.0);
    CHECK(channel(bright, 0) > channel(dark, 0) + 80);

    // A compile error: logged, cached as a failure, the material renders with built-in PBR (white).
    s.write_shader("void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = undefined_thing;\n}\n");
    changes = reloader.check();
    REQUIRE(changes.shaders.size() == 1);
    runtime::AssetHotReloader::apply(changes, &cache, nullptr);
    CHECK_FALSE(cache.surface_shader(s.shader).ok());
    mat = cache.material(s.material);
    REQUIRE(mat.ok());
    CHECK_FALSE(mat->surface_shader);
    CHECK(is_white(centre(s, cache)));

    // Fixed again: the custom look returns.
    s.write_shader("void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = i.params[0].rgb;\n}\n");
    runtime::AssetHotReloader::apply(reloader.check(), &cache, nullptr);
    CHECK(is_blue(centre(s, cache)));
    cache.clear();
}
