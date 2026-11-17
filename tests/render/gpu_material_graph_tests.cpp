// SPDX-License-Identifier: Apache-2.0
// Material graphs (.vmatgraph, ADR-0062): every node compiles to valid surface-shader GLSL, generic
// math widens to the widest input, diagnostics catch broken graphs, and a material using a graph
// renders through the surface shader path and hot reloads.
#include "gpu_fixture.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/script/material_graph.hpp"

#include <chrono>
#include <filesystem>

using namespace victus;
using namespace victus::scene;
using script::GraphDomain;
using script::ScriptGraph;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;
namespace sfs = std::filesystem;

namespace {

ScriptGraph material_graph() {
    ScriptGraph g;
    g.domain = GraphDomain::Material;
    return g;
}

bool has_error(const std::vector<script::GraphDiagnostic>& d, std::string_view text) {
    for (const auto& x : d)
        if (x.severity == script::GraphDiagnostic::Severity::Error && x.message.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("material graphs: every node compiles to a valid surface shader") {
    CHECK(assets::kind_for_extension(".vmatgraph") == assets::AssetKind::Shader);
    std::size_t nodes = 0;
    for (const auto& d : script::material_node_library()) {
        if (d.type == script::kMaterialOutputNode) continue;
        ++nodes;
        CAPTURE(d.type);
        REQUIRE_FALSE(d.outputs.empty());
        // Each output feeds a different Surface field (the compiler converts between widths).
        for (const auto& out_pin : d.outputs) {
            for (const char* field : {"base_color", "roughness", "emissive", "normal"}) {
                ScriptGraph g = material_graph();
                const auto n = g.add_node(d.type, 0, 0);
                const auto out = g.add_node(std::string(script::kMaterialOutputNode), 300, 0);
                REQUIRE(g.connect(n, out_pin.name, out, field));
                const auto compiled = script::compile_material_graph(g, "test.vmatgraph");
                CAPTURE(out_pin.name);
                CAPTURE(field);
                INFO(compiled.error_text());
                REQUIRE(compiled.ok());
                auto shader = runtime::compile_surface_shader(compiled.source, "test.vmatgraph", false);
                const std::string why = shader.ok() ? std::string() : shader.error().message + "\n" + compiled.source;
                INFO(why);
                REQUIRE(shader.ok());
            }
        }
    }
    CHECK(nodes > 40);
    // The starter graph compiles and round-trips through its file format with the domain intact.
    const ScriptGraph starter = script::starter_material_graph();
    const std::string text = script::serialize_graph(starter);
    CHECK(text.find("\"domain\": \"material\"") != std::string::npos);
    auto back = script::parse_graph(text);
    REQUIRE(back.ok());
    CHECK(back->domain == GraphDomain::Material);
    CHECK(script::serialize_graph(*back) == text);
    auto glsl = script::material_graph_to_glsl(text, "shaders/Starter.vmatgraph");
    REQUIRE(glsl.ok());
    CHECK(glsl->find("Generated from shaders/Starter.vmatgraph") != std::string::npos);
    CHECK(runtime::compile_surface_shader(*glsl, "starter", true).ok());
    // Script graphs keep their exact file format.
    CHECK(script::serialize_graph(ScriptGraph{}).find("domain") == std::string::npos);
}

TEST_CASE("material graphs: generic types, constants on the output and unused nodes") {
    ScriptGraph g = material_graph();
    const auto t = g.add_node("mat.time");
    const auto pos = g.add_node("mat.world_position");
    const auto mul = g.add_node("mat.multiply");
    const auto sin = g.add_node("mat.sin");
    const auto out = g.add_node(std::string(script::kMaterialOutputNode));
    const auto unused = g.add_node("mat.noise");
    REQUIRE(g.connect(t, "seconds", mul, "a"));
    REQUIRE(g.connect(pos, "position", mul, "b"));
    REQUIRE(g.connect(mul, "result", sin, "x"));
    REQUIRE(g.connect(sin, "result", out, "emissive"));
    g.node(out)->inputs.set("roughness", json::Value(0.25));
    g.node(out)->inputs.set("unlit", json::Value(true));
    const auto c = script::compile_material_graph(g);
    INFO(c.error_text());
    REQUIRE(c.ok());
    // float * vec3 widens to vec3; sin follows.
    CHECK(c.source.find(std::format("vec3 n{}_result = (vec3(n{}_seconds) * n{}_position);", mul, t, pos)) != std::string::npos);
    CHECK(c.source.find(std::format("vec3 n{}_result = sin(n{}_result);", sin, mul)) != std::string::npos);
    CHECK(c.source.find(std::format("s.emissive = n{}_result;", sin)) != std::string::npos);
    CHECK(c.source.find("s.roughness = 0.25;") != std::string::npos);
    CHECK(c.source.find("s.unlit = true;") != std::string::npos);
    CHECK(c.source.find("s.base_color") == std::string::npos); // unlinked, no constant: material value kept
    CHECK(c.source.find("victus_mg_noise") == std::string::npos); // the unused noise node adds nothing
    bool warned = false;
    for (const auto& d : c.diagnostics) warned |= d.node == unused && d.severity == script::GraphDiagnostic::Severity::Warning;
    CHECK(warned);
    CHECK(runtime::compile_surface_shader(c.source, "generic", false).ok());
    // Deterministic output.
    CHECK(script::compile_material_graph(g).source == c.source);
}

TEST_CASE("material graphs: diagnostics") {
    // No output.
    ScriptGraph g = material_graph();
    (void)g.add_node("mat.time");
    CHECK(has_error(script::validate_material_graph(g), "Surface Output"));
    // Two outputs.
    (void)g.add_node(std::string(script::kMaterialOutputNode));
    (void)g.add_node(std::string(script::kMaterialOutputNode));
    CHECK(has_error(script::validate_material_graph(g), "exactly one"));
    // A script node, a cycle.
    ScriptGraph h = material_graph();
    const auto out = h.add_node(std::string(script::kMaterialOutputNode));
    const auto log = h.add_node("debug.log");
    const auto a = h.add_node("mat.add");
    const auto b = h.add_node("mat.add");
    h.links.push_back({a, "result", b, "a"});
    h.links.push_back({b, "result", a, "a"});
    h.links.push_back({b, "result", out, "alpha"});
    auto d = script::validate_material_graph(h);
    CHECK(has_error(d, "unknown material node type 'debug.log'"));
    h.remove_node(log);
    d = script::validate_material_graph(h);
    CHECK(has_error(d, "feeds back into itself"));
    // validate_graph dispatches on the domain; the script compiler refuses material graphs.
    CHECK(has_error(script::validate_graph(h), "feeds back"));
    CHECK_FALSE(script::compile_graph(h).ok());
    // Bool pins only take bools; numeric types convert.
    ScriptGraph k = material_graph();
    const auto o = k.add_node(std::string(script::kMaterialOutputNode));
    const auto col = k.add_node("mat.color");
    CHECK_FALSE(k.connect(col, "alpha", o, "unlit"));
    CHECK(k.connect(col, "color", o, "base_color"));
    CHECK(script::pin_types_compatible(script::PinType::Vec4, script::PinType::Number, GraphDomain::Material));
    CHECK_FALSE(script::pin_types_compatible(script::PinType::Vec4, script::PinType::Number));
    // Errors name the file and node.
    auto bad = script::material_graph_to_glsl(script::serialize_graph(h), "shaders/bad.vmatgraph");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("shaders/bad.vmatgraph: node") != std::string::npos);
    CHECK(bad.error().message.find("(Add)") != std::string::npos);
}

TEST_CASE("gpu: a material graph shades a mesh and hot reloads") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const fs::Path project = sfs::temp_directory_path() / ("victus_matgraph_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(project / "assets" / "shaders");
    auto opened = assets::AssetDatabase::open(project);
    REQUIRE(opened.ok());
    auto db = std::move(opened).value();
    int bump = 0;
    const fs::Path file = project / "assets" / "shaders" / "tint.vmatgraph";
    auto write = [&](const ScriptGraph& g) {
        const std::string text = script::serialize_graph(g);
        REQUIRE(fs::write_text_atomic(file, text).ok());
        sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(++bump * 5));
        (void)db->refresh();
    };
    // Parameter 0 (blue) -> base colour.
    ScriptGraph g = material_graph();
    const auto param = g.add_node("mat.param0");
    const auto out = g.add_node(std::string(script::kMaterialOutputNode));
    REQUIRE(g.connect(param, "rgb", out, "base_color"));
    write(g);
    const Uuid shader = db->find_path("shaders/tint.vmatgraph")->uuid;
    assets::MaterialAsset m;
    m.unlit = true;
    m.base_color = {1, 1, 1, 1};
    m.shader = shader;
    m.shader_params[0] = {0, 0, 1, 0};
    REQUIRE(assets::save_material_file(project / "assets" / "glow.vmat", m).ok());
    (void)db->refresh();
    const Uuid material = db->find_path("glow.vmat")->uuid;
    auto code = runtime::load_surface_shader_code(*db, shader);
    REQUIRE(code.ok());
    CHECK(code->find("i.params[0].rgb") != std::string::npos);

    Scene scene;
    const Entity cam = scene.create("Camera");
    scene.world().get<Transform>(cam)->position = {0, 6, 0};
    scene.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
    scene.world().add<CameraComponent>(cam);
    const Entity plane = scene.create("Floor");
    scene.world().get<Transform>(plane)->scale = {20, 1, 20};
    auto& mr = scene.world().add<MeshRendererComponent>(plane);
    mr.primitive = "plane";
    mr.material_asset = material;
    scene.update_transforms();

    runtime::GpuAssetCache cache(*gpu().ctx, db.get());
    auto centre = [&]() {
        auto& dev = gpu().ctx->device();
        auto sr = render::SceneRenderer::create(*gpu().ctx);
        REQUIRE(sr.ok());
        constexpr std::uint32_t W = 96, H = 64;
        auto target = test::make_target(W, H);
        (void)runtime::extract_scene(scene, cache, **sr, {});
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        auto px = dev.read_texture(target);
        REQUIRE(px.ok());
        test::save_output("material_graph", W, H, *px);
        dev.destroy(target);
        return pixel(*px, W, W / 2, H / 2);
    };
    auto mat = cache.material(material);
    REQUIRE(mat.ok());
    CHECK(mat->surface_shader);
    const auto blue = centre();
    CHECK(channel(blue, 2) > 150);
    CHECK(channel(blue, 0) < 60);

    // Edit: a red Color constant instead; the reloader recompiles the graph.
    runtime::AssetHotReloader reloader(*db);
    const auto red = g.add_node("mat.color");
    g.node(red)->inputs.set("color", json::Value(json::Array{json::Value(1.0), json::Value(0.0), json::Value(0.0), json::Value(1.0)}));
    REQUIRE(g.connect(red, "rgb", out, "base_color"));
    write(g);
    auto changes = reloader.check();
    REQUIRE(changes.shaders.size() == 1);
    runtime::AssetHotReloader::apply(changes, &cache, nullptr);
    const auto now_red = centre();
    CHECK(channel(now_red, 0) > 150);
    CHECK(channel(now_red, 2) < 60);

    // A broken graph (no output) falls back to built-in PBR: the white base colour.
    g.remove_node(out);
    write(g);
    runtime::AssetHotReloader::apply(reloader.check(), &cache, nullptr);
    auto failed = cache.surface_shader(shader);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message.find("Surface Output") != std::string::npos);
    const auto white = centre();
    CHECK(channel(white, 0) > 150);
    CHECK(channel(white, 2) > 150);
    cache.clear();
    db.reset();
    std::error_code ec;
    sfs::remove_all(project, ec);
}
