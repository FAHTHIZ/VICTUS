// SPDX-License-Identifier: Apache-2.0
// Run-time shader compilation (glslang + SPIRV-Cross): every built-in shader compiles and
// reflects to the same resource counts the offline generator embedded, errors carry line numbers.
#include "victus/core/fs.hpp"
#include "victus/render/builtin_shaders.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/shadercompiler/shader_compiler.hpp"

#include "doctest.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

using namespace victus;
namespace sc = victus::shadercompiler;

namespace {
// Sorted slot numbers of every `[[buffer(N)]]` / `[[texture(N)]]` attribute in MSL source.
std::vector<int> metal_slots(std::string_view msl, std::string_view tag) {
    std::vector<int> out;
    for (std::size_t p = msl.find(tag); p != std::string_view::npos; p = msl.find(tag, p + 1))
        out.push_back(std::atoi(std::string(msl.substr(p + tag.size(), 4)).c_str()));
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

TEST_CASE("shader compiler: built-in shaders compile with the generator's resource counts") {
    int compiled = 0;
    for (const auto& b : render::builtin_shaders()) {
        const std::filesystem::path file = std::filesystem::path(VICTUS_SHADER_SOURCE_DIR) / std::string(b.name);
        auto text = fs::read_text(file);
        REQUIRE_MESSAGE(text.ok(), std::string(b.name));
        if (b.stage == rhi::ShaderStage::Compute) {
            // Built-in compute shaders only come from the offline library (the runtime compiler
            // serves surface shaders); they must still compile, with their workgroup size recorded.
            sc::CompileOptions o;
            o.name = std::string(b.name);
            o.want_msl = false;
            auto r = sc::compile_glsl(*text, sc::Stage::Compute, o);
            REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
            CHECK(b.threads[0] * b.threads[1] * b.threads[2] > 0);
            CHECK(b.readwrite_storage_buffers > 0);
            continue;
        }
        const sc::Stage stage = b.stage == rhi::ShaderStage::Vertex ? sc::Stage::Vertex : sc::Stage::Fragment;
        sc::CompileOptions o;
        o.name = std::string(b.name);
        auto r = sc::compile_glsl(*text, stage, o);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        CHECK(r->spirv.size() % 4 == 0);
        CHECK(r->spirv[0] == 0x03); // SPIR-V magic 0x07230203, little endian
        CHECK(r->spirv[3] == 0x07);
        CHECK(r->msl.find("using namespace metal;") != std::string::npos);
        INFO(b.name);
        CHECK(r->counts.samplers == b.samplers);
        CHECK(r->counts.uniform_buffers == b.uniform_buffers);
        CHECK(r->counts.storage_buffers == b.storage_buffers);
        CHECK(r->counts.storage_textures == b.storage_textures);
        // Metal: the run-time compiler's buffer slots match the offline library (SDL_GPU order:
        // uniform buffers, then storage buffers), so surface shaders bind like built-ins.
        CHECK(metal_slots(r->msl, "[[buffer(") == metal_slots(b.msl, "[[buffer("));
        CHECK(metal_slots(r->msl, "[[texture(") == metal_slots(b.msl, "[[texture("));
        auto again = sc::reflect(r->spirv);
        REQUIRE(again.ok());
        CHECK(*again == r->counts);
        ++compiled;
    }
    CHECK(compiled >= 20);
}

TEST_CASE("shader compiler: errors report the line, defines reach the source") {
    const char* src = "#version 450\n"
                      "layout(location = 0) out vec4 o;\n"
                      "void main() {\n"
                      "    o = vec4(undefined_name);\n"
                      "}\n";
    std::vector<sc::Diagnostic> diags;
    sc::CompileOptions o;
    o.name = "broken.frag";
    auto r = sc::compile_glsl(src, sc::Stage::Fragment, o, &diags);
    REQUIRE_FALSE(r.ok());
    REQUIRE_FALSE(diags.empty());
    CHECK(diags[0].line == 4);
    CHECK(diags[0].message.find("undefined_name") != std::string::npos);
    CHECK(r.error().message.find("broken.frag:4:") != std::string::npos);

    const char* uses_define = "#version 450\n"
                              "layout(location = 0) out vec4 o;\n"
                              "void main() { o = vec4(BRIGHTNESS); }\n";
    CHECK_FALSE(sc::compile_glsl(uses_define, sc::Stage::Fragment).ok());
    o.defines = {{"BRIGHTNESS", "0.5"}};
    o.want_msl = false;
    auto ok = sc::compile_glsl(uses_define, sc::Stage::Fragment, o);
    REQUIRE(ok.ok());
    CHECK(ok->msl.empty());
    CHECK_FALSE(sc::reflect({1, 2, 3}).ok());
}

TEST_CASE("shader compiler: surface shaders splice into pbr.frag with their own line numbers") {
    const std::string ok_code = "// tint by height\n"
                                "void surface(SurfaceInput i, inout Surface s) {\n"
                                "    s.base_color *= 0.5 + 0.5 * sin(i.time + i.world_position.y);\n"
                                "    s.emissive += i.params[0].rgb;\n"
                                "}\n";
    sc::CompileOptions o;
    o.name = "glow.vshader";
    auto r = sc::compile_glsl(render::surface_shader_source(ok_code), sc::Stage::Fragment, o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const auto* pbr = render::find_builtin_shader("pbr.frag");
    REQUIRE(pbr);
    CHECK(r->counts.samplers == pbr->samplers);
    CHECK(r->counts.uniform_buffers == pbr->uniform_buffers);

    const std::string bad_code = "void surface(SurfaceInput i, inout Surface s) {\n"
                                 "    s.base_color = vec3(1.0);\n"
                                 "    s.glow = 2.0;\n"
                                 "}\n";
    std::vector<sc::Diagnostic> diags;
    auto bad = sc::compile_glsl(render::surface_shader_source(bad_code), sc::Stage::Fragment, o, &diags);
    REQUIRE_FALSE(bad.ok());
    REQUIRE_FALSE(diags.empty());
    CHECK(diags[0].line == 3);
    CHECK(diags[0].message.find("glow") != std::string::npos);

    // Code after the splice keeps the base file's numbering (errors there would point at pbr.frag).
    const std::string src = render::surface_shader_source("");
    CHECK(src.find("#define VICTUS_SURFACE_SHADER 1") != std::string::npos);
    CHECK(render::pbr_fragment_source().find("// VICTUS_SURFACE_CODE") != std::string_view::npos);
    // A surface function is required once the hook is enabled.
    CHECK_FALSE(sc::compile_glsl(src, sc::Stage::Fragment, o).ok());
}
