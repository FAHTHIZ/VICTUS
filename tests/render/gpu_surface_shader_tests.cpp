// SPDX-License-Identifier: Apache-2.0
// Project surface shaders (.vshader) end to end: a material referencing a shader asset is
// compiled at load time and changes what reaches the screen, per-material parameters and time
// reach the shader, broken shaders fall back to built-in PBR, and hot reload recompiles them.
#include "gpu_fixture.hpp"
#include "victus/render/image.hpp"
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

std::uint32_t g_surface_draws = 0; // SceneStats::surface_shader_draws of the last centre() frame

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
    g_surface_draws = (*sr)->stats().surface_shader_draws;
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
    CHECK(g_surface_draws == 1);

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
    CHECK(g_surface_draws == 0); // the fallback is counted as built-in shading

    // Fixed again: the custom look returns.
    s.write_shader("void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = i.params[0].rgb;\n}\n");
    runtime::AssetHotReloader::apply(reloader.check(), &cache, nullptr);
    CHECK(is_blue(centre(s, cache)));
    cache.clear();
}

namespace {
// Mean of the red channel over the image (RGBA8).
double mean_red(const std::vector<std::uint8_t>& px) {
    double sum = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) sum += px[i];
    return sum / static_cast<double>(px.size() / 4);
}
} // namespace

TEST_CASE("gpu: surface shader vertex functions move geometry in the main and shadow passes") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& dev = gpu().ctx->device();
    // Code checks first: function detection ignores comments and prototypes.
    CHECK(render::surface_code_has_function("void vertex(VertexInput i, inout Vertex v) { }", "vertex"));
    CHECK_FALSE(render::surface_code_has_function("// void vertex(VertexInput i, inout Vertex v) { }", "vertex"));
    CHECK_FALSE(render::surface_code_has_function("void vertex(VertexInput i, inout Vertex v);", "vertex"));
    CHECK_FALSE(render::surface_code_has_function("void vertexy(VertexInput i) {}", "vertex"));
    const std::string both = "void surface(SurfaceInput i, inout Surface s) {\n s.base_color = vec3(1);\n}\nvoid vertex(VertexInput i, inout Vertex v) {\n}\n";
    const std::string no_vertex = render::surface_code_without_function(both, "vertex");
    CHECK(std::count(no_vertex.begin(), no_vertex.end(), '\n') == std::count(both.begin(), both.end(), '\n'));
    CHECK(no_vertex.find("vertex") == std::string::npos);
    CHECK(no_vertex.find("surface") != std::string::npos);
    // Vertex-only shaders compile (built-in surface), errors carry the user's line, resources are refused.
    CHECK(runtime::compile_surface_shader("void vertex(VertexInput i, inout Vertex v) {\n    v.world_position.y += 1.0;\n}\n", "ok", false).ok());
    auto bad = runtime::compile_surface_shader("void vertex(VertexInput i, inout Vertex v) {\n\n    v.world_position = nope;\n}\n", "wave.vshader", false);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("wave.vshader:3") != std::string::npos);
    auto res = runtime::compile_surface_shader("layout(set = 0, binding = 0) uniform sampler2D extra;\n"
                                               "void vertex(VertexInput i, inout Vertex v) { v.world_position.y += texture(extra, i.uv).r; }\n",
                                               "res", false);
    REQUIRE_FALSE(res.ok());

    ShaderScene s;
    // Main pass: a 20 m plane shifted 10 m along +x by params[0].x leaves half the view empty.
    s.write_shader("void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = i.params[0].yzw;\n}\n"
                   "void vertex(VertexInput i, inout Vertex v) {\n    v.world_position.x += i.params[0].x;\n}\n");
    assets::MaterialAsset m;
    m.unlit = true;
    m.shader = s.shader;
    m.shader_params[0] = {0, 0, 0, 1};
    REQUIRE(assets::save_material_file(s.project / "assets" / "glow.vmat", m).ok());
    (void)s.db->refresh();
    constexpr std::uint32_t W = 96, H = 64;
    auto frame = [&](runtime::GpuAssetCache& cache) {
        auto sr = render::SceneRenderer::create(*gpu().ctx);
        REQUIRE(sr.ok());
        (*sr)->set_time(0.0);
        auto target = test::make_target(W, H);
        (void)runtime::extract_scene(s.scene, cache, **sr, {});
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target);
        REQUIRE(px.ok());
        dev.destroy(target);
        return *px;
    };
    {
        runtime::GpuAssetCache cache(*gpu().ctx, s.db.get());
        REQUIRE(cache.surface_vertex_stage(s.shader));
        auto still = frame(cache);
        CHECK(is_blue(pixel(still, W, W / 4, H / 2)));
        CHECK(is_blue(pixel(still, W, 3 * W / 4, H / 2)));
        m.shader_params[0] = {10, 0, 0, 1};
        REQUIRE(assets::save_material_file(s.project / "assets" / "glow.vmat", m).ok());
        (void)s.db->refresh();
        cache.evict_materials();
        auto moved = frame(cache);
        test::save_output("surface_vertex_shift", W, H, moved);
        const bool left = is_blue(pixel(moved, W, W / 4, H / 2)), right = is_blue(pixel(moved, W, 3 * W / 4, H / 2));
        CHECK(left != right);
        cache.clear();
    }

    // Shadow pass: a cube over a lit floor under an overhead sun hides its own shadow; moving it
    // 2 m sideways only while drawing shadows uncovers a dark patch on the floor.
    Scene& sc = s.scene;
    auto* floor_mr = sc.world().get<MeshRendererComponent>(s.plane);
    floor_mr->material_asset = Uuid{};
    const Entity sun = sc.create("Sun");
    sc.world().get<Transform>(sun)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
    sc.world().add<LightComponent>(sun).intensity = 3.0f;
    const Entity cube = sc.create("Cube");
    sc.world().get<Transform>(cube)->position = {0, 1.5f, 0};
    auto& cmr = sc.world().add<MeshRendererComponent>(cube);
    cmr.primitive = "cube";
    cmr.material_asset = s.material;
    sc.update_transforms();
    m.unlit = false;
    m.shader_params[0] = {0, 0, 0, 1};
    s.write_shader("void vertex(VertexInput i, inout Vertex v) {\n    if (i.shadow) v.world_position.x += i.params[0].x;\n}\n");
    m.shader = s.shader;
    REQUIRE(assets::save_material_file(s.project / "assets" / "glow.vmat", m).ok());
    (void)s.db->refresh();
    runtime::GpuAssetCache cache(*gpu().ctx, s.db.get());
    // The camera looks straight down from 6 m: floor x = +2 m lands near pixel (66, 32).
    auto red_at = [&](const std::vector<std::uint8_t>& px) { return double(px[(std::size_t(H / 2) * W + 66) * 4]); };
    const auto still = frame(cache);
    const double hidden = red_at(still), hidden_mean = mean_red(still);
    m.shader_params[0] = {2, 0, 0, 1};
    REQUIRE(assets::save_material_file(s.project / "assets" / "glow.vmat", m).ok());
    (void)s.db->refresh();
    cache.evict_materials();
    auto shadowed = frame(cache);
    test::save_output("surface_vertex_shadow", W, H, shadowed);
    const double uncovered = red_at(shadowed);
    MESSAGE("red at x=+2 m: shadow under the cube " << hidden << ", shadow moved out " << uncovered);
    CHECK(uncovered < hidden - 30.0);
    CHECK(mean_red(shadowed) < hidden_mean);
    cache.clear();
}

TEST_CASE("gpu: surface shader named parameters and textures reach both stages") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& dev = gpu().ctx->device();
    // Declarations: slots in order, hints, defaults; comments are ignored.
    const auto d = render::parse_surface_declarations("uniform float wind : hint_range(0, 2, 0.5) = 0.5;\n"
                                                      "// uniform float nope = 1.0;\n"
                                                      "uniform vec3 tint : source_color = vec3(1, 0.5, 0);\n"
                                                      "uniform vec2 scroll = vec2(2);\n"
                                                      "uniform sampler2D mask : hint_default_black;\n"
                                                      "uniform sampler2D bumps : hint_normal;\n"
                                                      "void surface(SurfaceInput i, inout Surface s) {}\n");
    REQUIRE(d.ok());
    REQUIRE(d.params.size() == 3);
    CHECK(d.params[0].name == "wind");
    CHECK(d.params[0].components == 1);
    CHECK(d.params[0].has_range);
    CHECK(d.params[0].max == 2.0f);
    CHECK(d.params[0].step == 0.5f);
    CHECK(d.params[0].value.x == 0.5f);
    CHECK(d.params[1].slot == 1);
    CHECK(d.params[1].color);
    CHECK(d.params[1].value == math::Vec4{1, 0.5f, 0, 0});
    CHECK(d.params[1].line == 3);
    CHECK(d.params[2].value == math::Vec4{2, 2, 0, 0});
    REQUIRE(d.textures.size() == 2);
    CHECK(d.textures[0].fallback == render::SurfaceTextureDefault::Black);
    CHECK(d.textures[1].fallback == render::SurfaceTextureDefault::Normal);
    CHECK(d.textures[1].slot == 1);
    for (const char* bad : {"uniform float a = 1;\nuniform float a = 2;\n", "uniform float a : hint_rnge(0, 1);\n",
                            "uniform vec3 c = vec2(1, 2);\n", "uniform float u_x;\n", "uniform sampler2D t = 1;\n",
                            "uniform float a;\nuniform float b;\nuniform float c;\nuniform float d;\nuniform float e;\n",
                            "uniform float a : source_color;\n"})
        CHECK_FALSE(render::parse_surface_declarations(bad).ok());
    auto dup = runtime::compile_surface_shader("void surface(SurfaceInput i, inout Surface s) {}\nuniform float a;\nuniform float a;\n",
                                               "dup.vshader", false);
    REQUIRE_FALSE(dup.ok());
    CHECK(dup.error().message.find("dup.vshader:3") != std::string::npos);
    // GLES: one shader texture (16 texture units per stage).
    auto gles = runtime::compile_surface_shader("uniform sampler2D a;\nuniform sampler2D b;\nvoid surface(SurfaceInput i, inout Surface s) {}\n",
                                                "two.vshader", rhi::Backend::GLES);
    REQUIRE_FALSE(gles.ok());
    CHECK(gles.error().message.find("two.vshader:2") != std::string::npos);
    CHECK(runtime::compile_surface_shader("uniform sampler2D a;\nvoid surface(SurfaceInput i, inout Surface s) { s.base_color = texture(a, i.uv).rgb; }\n",
                                          "one.vshader", rhi::Backend::GLES)
              .ok());
    // Four textures, used only by the vertex function, still compile for desktop backends.
    CHECK(runtime::compile_surface_shader("uniform sampler2D a;\nuniform sampler2D b;\nuniform sampler2D c;\nuniform sampler2D e;\n"
                                          "void vertex(VertexInput i, inout Vertex v) {\n"
                                          "    v.world_position.y += textureLod(a, i.uv, 0.0).r + textureLod(e, i.uv, 0.0).r;\n}\n",
                                          "four.vshader", false)
              .ok());

    ShaderScene s;
    auto png = render::encode_png(render::make_checker(4, 1, 0xff0000ffu, 0xff0000ffu)); // solid red
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(s.project / "assets" / "red.png", *png).ok());
    (void)s.db->refresh();
    const Uuid red = s.db->find_path("red.png")->uuid;
    // tint (default blue) + mask (default black); the vertex function moves the plane by 10 m times
    // the mask's red channel, so a red mask both colours and shifts it.
    s.write_shader("uniform vec3 tint : source_color = vec3(0, 0, 1);\n"
                   "uniform sampler2D mask : hint_default_black;\n"
                   "void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = tint + texture(mask, i.mesh_uv).rgb;\n}\n"
                   "void vertex(VertexInput i, inout Vertex v) {\n    v.world_position.x += textureLod(mask, vec2(0.5), 0.0).r * 10.0;\n}\n");
    assets::MaterialAsset m;
    m.unlit = true;
    m.shader = s.shader;
    REQUIRE(assets::save_material_file(s.project / "assets" / "glow.vmat", m).ok());
    (void)s.db->refresh();
    constexpr std::uint32_t W = 96, H = 64;
    auto frame = [&](runtime::GpuAssetCache& cache, const char* name) {
        auto sr = render::SceneRenderer::create(*gpu().ctx);
        REQUIRE(sr.ok());
        auto target = test::make_target(W, H);
        (void)runtime::extract_scene(s.scene, cache, **sr, {});
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target);
        REQUIRE(px.ok());
        test::save_output(name, W, H, *px);
        dev.destroy(target);
        return *px;
    };
    runtime::GpuAssetCache cache(*gpu().ctx, s.db.get());
    const auto* decl = cache.surface_declarations(s.shader);
    REQUIRE(decl);
    CHECK(decl->params.size() == 1);
    CHECK(decl->textures.size() == 1);
    {
        auto mat = cache.material(s.material);
        REQUIRE(mat.ok());
        CHECK(mat->shader_texture_count == 1);
        CHECK(mat->shader_params[0] == math::Vec4{0, 0, 1, 0}); // declared default
        auto px = frame(cache, "surface_named_default");
        CHECK(is_blue(pixel(px, W, W / 4, H / 2)));
        CHECK(is_blue(pixel(px, W, 3 * W / 4, H / 2)));
    }
    // A material value overrides the default (green), and a texture fills the mask.
    m.shader_params[0] = {0, 1, 0, 0};
    m.shader_textures[0] = red;
    REQUIRE(assets::save_material_file(s.project / "assets" / "glow.vmat", m).ok());
    (void)s.db->refresh();
    auto loaded = assets::load_material_file(s.project / "assets" / "glow.vmat");
    REQUIRE(loaded.ok());
    CHECK(loaded->shader_textures[0] == red);
    cache.evict_materials();
    for (int i = 0; i < 50 && !cache.material(s.material)->shader_textures[0]; ++i) {
        cache.pump();
        cache.evict_materials();
    }
    auto px = frame(cache, "surface_named_texture");
    const std::uint32_t left = pixel(px, W, W / 4, H / 2), right = pixel(px, W, 3 * W / 4, H / 2);
    MESSAGE("left " << std::hex << left << " right " << right);
    // Shifted 10 m along +x: one half shows the clear colour, the other red + green (yellow).
    const auto yellow = [](std::uint32_t p) { return channel(p, 0) > 150 && channel(p, 1) > 150 && channel(p, 2) < 60; };
    CHECK(yellow(left) != yellow(right));
    CHECK((yellow(left) || yellow(right)));
    cache.clear();
}
