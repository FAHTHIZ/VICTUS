// SPDX-License-Identifier: Apache-2.0
#include "victus/shadercompiler/shader_compiler.hpp"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace victus::shadercompiler {
namespace {

void ensure_initialized() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

EShLanguage language(Stage s) {
    switch (s) {
    case Stage::Vertex: return EShLangVertex;
    case Stage::Fragment: return EShLangFragment;
    case Stage::Compute: return EShLangCompute;
    }
    return EShLangFragment;
}

// glslang reports "ERROR: <string>:<line>: '<token>' : <message>"; keep the line and the rest.
void parse_log(const char* log, std::vector<Diagnostic>& out) {
    std::istringstream in(log ? log : "");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::string_view v = line;
        bool error = false;
        for (std::string_view prefix : {"ERROR: ", "WARNING: "}) {
            if (v.starts_with(prefix)) {
                error = prefix == "ERROR: ";
                v.remove_prefix(prefix.size());
            }
        }
        if (!error) continue;
        if (v.find("compilation terminated") != std::string_view::npos) continue;
        if (v.find("compilation errors.") != std::string_view::npos) continue;
        Diagnostic d;
        // "<string>:<line>: rest"
        const auto c1 = v.find(':');
        if (c1 != std::string_view::npos) {
            const auto c2 = v.find(':', c1 + 1);
            if (c2 != std::string_view::npos) {
                std::uint32_t n = 0;
                bool digits = c2 > c1 + 1;
                for (char ch : v.substr(c1 + 1, c2 - c1 - 1)) {
                    if (ch < '0' || ch > '9') {
                        digits = false;
                        break;
                    }
                    n = n * 10 + static_cast<std::uint32_t>(ch - '0');
                }
                if (digits) {
                    d.line = n;
                    v.remove_prefix(c2 + 1);
                }
            }
        }
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        d.message = std::string(v);
        out.push_back(std::move(d));
    }
}

std::vector<std::uint32_t> words_of(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint32_t> w(bytes.size() / 4);
    if (!w.empty()) std::memcpy(w.data(), bytes.data(), w.size() * 4);
    return w;
}

ResourceCounts count(const spirv_cross::Compiler& c) {
    const auto res = c.get_shader_resources();
    ResourceCounts r;
    r.samplers = static_cast<std::uint32_t>(res.sampled_images.size() + res.separate_images.size());
    r.storage_textures = static_cast<std::uint32_t>(res.storage_images.size());
    r.storage_buffers = static_cast<std::uint32_t>(res.storage_buffers.size());
    r.uniform_buffers = static_cast<std::uint32_t>(res.uniform_buffers.size());
    return r;
}

// SDL_GPU binds Metal buffers as uniform buffers first, then storage buffers
// ([[buffer(uniforms + i)]]), but SPIR-V numbers storage buffers after the textures of their
// set. Renumber them so the MSL matches SDL's layout; textures already line up (sampled, then
// storage). The offline generator gets the same result from VICTUS_MSL bindings in the sources.
void remap_metal_storage_buffers(spirv_cross::CompilerMSL& msl) {
    const auto res = msl.get_shader_resources();
    std::vector<std::pair<std::uint32_t, spirv_cross::ID>> buffers;
    for (const auto& b : res.storage_buffers) buffers.emplace_back(msl.get_decoration(b.id, spv::DecorationBinding), b.id);
    std::sort(buffers.begin(), buffers.end());
    const auto uniforms = static_cast<std::uint32_t>(res.uniform_buffers.size());
    for (std::size_t i = 0; i < buffers.size(); ++i)
        msl.set_decoration(buffers[i].second, spv::DecorationBinding, uniforms + static_cast<std::uint32_t>(i));
}

} // namespace

Result<CompiledShader> compile_glsl(std::string_view source, Stage stage, const CompileOptions& options,
                                    std::vector<Diagnostic>* diagnostics) {
    ensure_initialized();
    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const std::string text(source);
    const char* strings[] = {text.c_str()};
    const char* names[] = {options.name.c_str()};
    shader.setStringsWithLengthsAndNames(strings, nullptr, names, 1);
    std::string preamble;
    for (const auto& [k, v] : options.defines) preamble += std::format("#define {} {}\n", k, v);
    shader.setPreamble(preamble.c_str());
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setEntryPoint("main");
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    std::vector<Diagnostic> diags;
    auto fail = [&](const char* what) -> Error {
        std::string msg = std::format("{}: {}", options.name, what);
        for (const Diagnostic& d : diags)
            msg += d.line ? std::format("\n{}:{}: {}", options.name, d.line, d.message) : std::format("\n{}: {}", options.name, d.message);
        if (diagnostics) *diagnostics = diags;
        return Error{ErrorCode::InvalidArgument, std::move(msg)};
    };
    if (!shader.parse(GetDefaultResources(), 450, ENoProfile, false, false, messages)) {
        parse_log(shader.getInfoLog(), diags);
        return fail("compile failed");
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        parse_log(program.getInfoLog(), diags);
        return fail("link failed");
    }
    std::vector<std::uint32_t> words;
    glslang::SpvOptions spv;
    spv.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), words, &spv);
    if (words.empty()) return fail("SPIR-V generation failed");

    CompiledShader out;
    out.spirv.resize(words.size() * 4);
    std::memcpy(out.spirv.data(), words.data(), out.spirv.size());
    try {
        spirv_cross::CompilerMSL msl(words);
        out.counts = count(msl);
        if (options.want_msl) {
            spirv_cross::CompilerMSL::Options mo;
            mo.set_msl_version(2, 1);
            mo.enable_decoration_binding = true; // same as tools/shaders/compile_shaders.py
            msl.set_msl_options(mo);
            remap_metal_storage_buffers(msl);
            out.msl = msl.compile();
        }
    } catch (const std::exception& e) {
        diags.push_back({0, e.what()});
        return fail("MSL translation failed");
    }
    if (diagnostics) diagnostics->clear();
    return out;
}

Result<ResourceCounts> reflect(const std::vector<std::uint8_t>& spirv) {
    if (spirv.size() < 20 || spirv.size() % 4 != 0) return make_error(ErrorCode::InvalidArgument, "not a SPIR-V module");
    try {
        spirv_cross::Compiler c(words_of(spirv));
        return count(c);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::InvalidArgument, std::format("invalid SPIR-V: {}", e.what()));
    }
}

} // namespace victus::shadercompiler
