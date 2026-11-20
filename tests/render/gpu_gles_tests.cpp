// SPDX-License-Identifier: Apache-2.0
// OpenGL ES 3.0 backend (ADR-0144 phase 1): draws through the public RHI on a GLES device and
// reads the pixels back. The shaders are hand-written GLSL ES 3.00 that follow the backend's
// conventions (clip space fixed up as spirv-cross --fixup-clipspace --flip-vert-y does, slots
// declared with "//!" comments), so the expected images match what the SDL_GPU backends produce
// from the equivalent SPIR-V.
#include "../assets/skinned_model.hpp"
#include "gpu_fixture.hpp"
#include "victus/render/gpu_context.hpp"
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace victus;

namespace {

// Separate GLES device sharing the SDL video subsystem the main GPU fixture initialised.
struct Gles {
    std::unique_ptr<rhi::Device> device;
    std::string failure;
};

Gles& gles() {
    static Gles g = [] {
        Gles r;
        if (!test::gpu().platform) {
            r.failure = "no platform: " + test::gpu().failure;
            return r;
        }
        rhi::DeviceDesc dd;
        dd.headless = true;
        dd.preferred_driver = "gles";
        auto d = rhi::Device::create(dd);
        if (!d) {
            r.failure = d.error().message;
            return r;
        }
        r.device = std::move(d).value();
        return r;
    }();
    return g;
}

// Only headless Linux CI promises a GLES driver (Mesa); elsewhere a missing driver is a skip.
bool gles_required() {
#if defined(__linux__) && !defined(__ANDROID__)
    return test::gpu_required();
#else
    return false;
#endif
}

#define VICTUS_REQUIRE_GLES_OR_SKIP()                                                       \
    do {                                                                                    \
        if (!gles().device) {                                                               \
            if (gles_required()) FAIL("GLES required but unavailable: " << gles().failure); \
            MESSAGE("skipping GLES test: " << gles().failure);                              \
            return;                                                                         \
        }                                                                                   \
    } while (0)

// Vertex: a 2D position plus an optional per-instance offset and colour.
constexpr const char* kVert = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_offset;
layout(location = 2) in vec4 a_color;
//! ubo 0 VertParams
layout(std140) uniform VertParams { vec4 depth; };
out vec4 v_color;
out vec2 v_uv;
void main() {
    vec2 p = a_pos + a_offset;
    v_color = a_color;
    v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); // v = 0 at the top, like D3D/Vulkan
    gl_Position = vec4(p, depth.x, 1.0);
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}
)";

constexpr const char* kFragColor = R"(#version 300 es
precision highp float;
//! ubo 0 FragParams
layout(std140) uniform FragParams { vec4 color; vec4 use_vertex; };
in vec4 v_color;
in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() { o_color = mix(color, v_color, use_vertex.x); }
)";

constexpr const char* kFragTexture = R"(#version 300 es
precision highp float;
//! sampler 0 u_tex
uniform highp sampler2D u_tex;
in vec4 v_color;
in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() { o_color = texture(u_tex, v_uv); }
)";

struct FragParams {
    float color[4];
    float use_vertex[4];
};
struct VertParams {
    float depth[4];
};

struct Vertex {
    float x, y;
};
struct Instance {
    float ox, oy;
    std::uint8_t rgba[4];
};

rhi::ShaderHandle shader(rhi::ShaderStage stage, const char* src) {
    rhi::ShaderDesc d;
    d.name = stage == rhi::ShaderStage::Vertex ? "gles_test_vs" : "gles_test_fs";
    d.stage = stage;
    d.glsl_es = src;
    d.uniform_buffers = 1;
    if (std::strstr(src, "//! sampler")) d.samplers = 1;
    auto s = gles().device->create_shader(d);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    return s.value();
}

rhi::PipelineHandle pipeline(const char* frag, rhi::Format color, rhi::BlendMode blend = rhi::BlendMode::Opaque,
                             rhi::Format depth = rhi::Format::Invalid) {
    rhi::PipelineDesc p;
    p.name = "gles_test";
    p.vertex = shader(rhi::ShaderStage::Vertex, kVert);
    p.fragment = shader(rhi::ShaderStage::Fragment, frag);
    p.vertex_buffers = {{0, sizeof(Vertex), false}, {1, sizeof(Instance), true}};
    p.attributes = {{0, 0, rhi::VertexFormat::Float2, 0},
                    {1, 1, rhi::VertexFormat::Float2, 0},
                    {2, 1, rhi::VertexFormat::UByte4Norm, 8}};
    p.cull = rhi::CullMode::Back; // the quads below are counter-clockwise in clip space (y up)
    p.color_targets = {{color, blend}};
    if (depth != rhi::Format::Invalid) {
        p.depth_format = depth;
        p.depth_test = true;
        p.depth_write = true;
        p.depth_compare = rhi::CompareOp::Less;
    }
    auto r = gles().device->create_pipeline(p);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return r.value();
}

template <typename T>
rhi::BufferHandle buffer(const std::vector<T>& data, std::uint32_t usage) {
    rhi::BufferDesc d;
    d.name = "gles_test_buffer";
    d.size = static_cast<std::uint32_t>(data.size() * sizeof(T));
    d.usage = usage;
    auto b = gles().device->create_buffer(d);
    REQUIRE(b.ok());
    auto cmd = gles().device->begin_commands();
    cmd.upload_buffer(b.value(), 0, {reinterpret_cast<const std::uint8_t*>(data.data()), d.size});
    cmd.submit_and_wait();
    return b.value();
}

rhi::TextureHandle target(std::uint32_t w, std::uint32_t h, rhi::Format f) {
    rhi::TextureDesc d;
    d.name = "gles_test_target";
    d.format = f;
    d.width = w;
    d.height = h;
    d.usage = (rhi::is_depth(f) ? rhi::TextureUsageDepthTarget : rhi::TextureUsageColorTarget) | rhi::TextureUsageSampled;
    auto t = gles().device->create_texture(d);
    REQUIRE(t.ok());
    return t.value();
}

// Quad covering clip-space x in [x0, x1], y in [y0, y1], counter-clockwise with y up.
std::vector<Vertex> quad(float x0, float y0, float x1, float y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }
const std::vector<std::uint16_t> kQuadIndices = {0, 1, 2, 0, 2, 3};

struct Draw {
    rhi::PipelineHandle pipe;
    rhi::BufferHandle vb, ib, instances;
    FragParams frag{};
    VertParams vert{};
    rhi::TextureHandle tex;
    rhi::SamplerHandle sampler;
    std::uint32_t instance_count{1}, first_instance{0};
    std::int32_t vertex_offset{0};
};

void record(rhi::RenderPass& pass, rhi::CommandBuffer& cmd, const Draw& d) {
    pass.bind_pipeline(d.pipe);
    cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, &d.vert, sizeof(d.vert));
    cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, &d.frag, sizeof(d.frag));
    pass.bind_vertex_buffer(0, d.vb);
    pass.bind_vertex_buffer(1, d.instances);
    pass.bind_index_buffer(d.ib, rhi::IndexType::U16);
    if (d.tex) pass.bind_texture(rhi::ShaderStage::Fragment, 0, d.tex, d.sampler);
    pass.draw_indexed(6, d.instance_count, 0, d.vertex_offset, d.first_instance);
}

rhi::BufferHandle no_offset_instance() {
    static rhi::BufferHandle b = buffer(std::vector<Instance>{{0, 0, {0, 0, 0, 0}}}, rhi::BufferUsageVertex);
    return b;
}

std::uint16_t half_bits(const std::vector<std::uint8_t>& px, std::size_t index) {
    std::uint16_t v;
    std::memcpy(&v, px.data() + index * 2, 2);
    return v;
}

} // namespace

TEST_CASE("gles: device reports the GLES backend without compute") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    const auto& caps = gles().device->capabilities();
    CHECK(caps.backend == rhi::Backend::GLES);
    CHECK_FALSE(caps.compute);
    CHECK(caps.max_texture_size >= 2048);
    CHECK(caps.driver_name.find("OpenGL ES") != std::string::npos);
    MESSAGE("GLES driver: " << caps.driver_name);
    CHECK(gles().device->supports_format(rhi::Format::RGBA8_UNORM, rhi::TextureUsageColorTarget));
    CHECK_FALSE(gles().device->supports_format(rhi::Format::BGRA8_UNORM, rhi::TextureUsageSampled));
    auto compute = gles().device->create_compute_pipeline({});
    CHECK_FALSE(compute.ok());
}

TEST_CASE("gles: clear and draw land with row 0 at the top of the target") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    const auto rt = target(4, 4, rhi::Format::RGBA8_UNORM);
    Draw d;
    d.pipe = pipeline(kFragColor, rhi::Format::RGBA8_UNORM);
    d.vb = buffer(quad(-1, 0, 1, 1), rhi::BufferUsageVertex); // upper half of clip space
    d.ib = buffer(kQuadIndices, rhi::BufferUsageIndex);
    d.instances = no_offset_instance();
    d.frag = {{1, 0, 0, 1}, {0, 0, 0, 0}};
    d.vert = {{0.5f, 0, 0, 0}};
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 1, 1}}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    record(pass, cmd, d);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt);
    REQUIRE(px.ok());
    for (std::uint32_t x = 0; x < 4; ++x) {
        CHECK(test::pixel(px.value(), 4, x, 0) == 0xff0000ffu); // red, top
        CHECK(test::pixel(px.value(), 4, x, 1) == 0xff0000ffu);
        CHECK(test::pixel(px.value(), 4, x, 2) == 0xffff0000u); // blue clear, bottom
        CHECK(test::pixel(px.value(), 4, x, 3) == 0xffff0000u);
    }
    CHECK(dev.stats().draw_calls >= 1);
}

TEST_CASE("gles: sampled texture keeps upload row order") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    rhi::TextureDesc td;
    td.name = "gles_test_texture";
    td.width = 2;
    td.height = 2;
    auto tex = dev.create_texture(td);
    REQUIRE(tex.ok());
    const std::vector<std::uint8_t> texels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    auto up = dev.begin_commands();
    up.upload_texture(tex.value(), texels);
    up.submit_and_wait();
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = sd.mip_filter = rhi::Filter::Nearest;
    sd.address_u = sd.address_v = rhi::AddressMode::ClampToEdge;
    auto sampler = dev.create_sampler(sd);
    REQUIRE(sampler.ok());

    const auto rt = target(2, 2, rhi::Format::RGBA8_UNORM);
    Draw d;
    d.pipe = pipeline(kFragTexture, rhi::Format::RGBA8_UNORM);
    d.vb = buffer(quad(-1, -1, 1, 1), rhi::BufferUsageVertex);
    d.ib = buffer(kQuadIndices, rhi::BufferUsageIndex);
    d.instances = no_offset_instance();
    d.tex = tex.value();
    d.sampler = sampler.value();
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    record(pass, cmd, d);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt);
    REQUIRE(px.ok());
    CHECK(px.value() == texels); // an identity blit: same rows, same order
}

TEST_CASE("gles: depth test keeps the nearer surface") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    const auto rt = target(2, 2, rhi::Format::RGBA8_UNORM);
    const auto depth = target(2, 2, rhi::Format::D32_FLOAT);
    const auto pipe = pipeline(kFragColor, rhi::Format::RGBA8_UNORM, rhi::BlendMode::Opaque, rhi::Format::D32_FLOAT);
    Draw near_draw, far_draw;
    near_draw.pipe = far_draw.pipe = pipe;
    near_draw.vb = far_draw.vb = buffer(quad(-1, -1, 1, 1), rhi::BufferUsageVertex);
    near_draw.ib = far_draw.ib = buffer(kQuadIndices, rhi::BufferUsageIndex);
    near_draw.instances = far_draw.instances = no_offset_instance();
    near_draw.frag = {{0, 1, 0, 1}, {0, 0, 0, 0}};
    near_draw.vert = {{0.2f, 0, 0, 0}};
    far_draw.frag = {{1, 0, 0, 1}, {0, 0, 0, 0}};
    far_draw.vert = {{0.8f, 0, 0, 0}};
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt}};
    rp.depth = {depth, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    record(pass, cmd, near_draw);
    record(pass, cmd, far_draw); // drawn second but further away: rejected
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt);
    REQUIRE(px.ok());
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(test::pixel(px.value(), 2, i % 2, i / 2) == 0xff00ff00u);
}

TEST_CASE("gles: RGBA16F targets read back as half floats") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    if (!dev.supports_format(rhi::Format::RGBA16_FLOAT, rhi::TextureUsageColorTarget)) {
        MESSAGE("skipping: no EXT_color_buffer_half_float / float");
        return;
    }
    const auto rt = target(2, 2, rhi::Format::RGBA16_FLOAT);
    Draw d;
    d.pipe = pipeline(kFragColor, rhi::Format::RGBA16_FLOAT);
    d.vb = buffer(quad(-1, -1, 1, 1), rhi::BufferUsageVertex);
    d.ib = buffer(kQuadIndices, rhi::BufferUsageIndex);
    d.instances = no_offset_instance();
    d.frag = {{1.5f, 0.25f, -2.0f, 1.0f}, {0, 0, 0, 0}};
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    record(pass, cmd, d);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt);
    REQUIRE(px.ok());
    REQUIRE(px.value().size() == 2u * 2u * 8u);
    CHECK(half_bits(px.value(), 0) == 0x3E00); // 1.5
    CHECK(half_bits(px.value(), 1) == 0x3400); // 0.25
    CHECK(half_bits(px.value(), 2) == 0xC000); // -2.0
    CHECK(half_bits(px.value(), 3) == 0x3C00); // 1.0
}

TEST_CASE("gles: indexed instanced draws honour vertex_offset and first_instance") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    const auto rt = target(4, 2, rhi::Format::RGBA8_UNORM);
    Draw d;
    d.pipe = pipeline(kFragColor, rhi::Format::RGBA8_UNORM);
    // Four junk vertices first: vertex_offset = 4 must skip them.
    auto verts = quad(5, 5, 6, 6);
    const auto left = quad(-1, -1, 0, 1);
    verts.insert(verts.end(), left.begin(), left.end());
    d.vb = buffer(verts, rhi::BufferUsageVertex);
    d.ib = buffer(kQuadIndices, rhi::BufferUsageIndex);
    // Instance 0 is junk; first_instance = 1 draws instance 1 (left, red) and 2 (right, green).
    d.instances = buffer(std::vector<Instance>{{9, 9, {255, 255, 255, 255}}, {0, 0, {255, 0, 0, 255}}, {1, 0, {0, 255, 0, 255}}},
                         rhi::BufferUsageVertex);
    d.frag = {{0, 0, 0, 1}, {1, 1, 1, 1}};
    d.instance_count = 2;
    d.first_instance = 1;
    d.vertex_offset = 4;
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    record(pass, cmd, d);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt);
    REQUIRE(px.ok());
    for (std::uint32_t y = 0; y < 2; ++y) {
        CHECK(test::pixel(px.value(), 4, 0, y) == 0xff0000ffu);
        CHECK(test::pixel(px.value(), 4, 1, y) == 0xff0000ffu);
        CHECK(test::pixel(px.value(), 4, 2, y) == 0xff00ff00u);
        CHECK(test::pixel(px.value(), 4, 3, y) == 0xff00ff00u);
    }
}

TEST_CASE("gles: alpha blending over the clear colour") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    const auto rt = target(2, 2, rhi::Format::RGBA8_UNORM);
    Draw d;
    d.pipe = pipeline(kFragColor, rhi::Format::RGBA8_UNORM, rhi::BlendMode::Alpha);
    d.vb = buffer(quad(-1, -1, 1, 1), rhi::BufferUsageVertex);
    d.ib = buffer(kQuadIndices, rhi::BufferUsageIndex);
    d.instances = no_offset_instance();
    d.frag = {{1, 0, 0, 0.5f}, {0, 0, 0, 0}};
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 1, 1}}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    record(pass, cmd, d);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt);
    REQUIRE(px.ok());
    const std::uint32_t p = test::pixel(px.value(), 2, 0, 0);
    CHECK(test::channel(p, 0) == doctest::Approx(128).epsilon(0.02));
    CHECK(test::channel(p, 1) == 0);
    CHECK(test::channel(p, 2) == doctest::Approx(128).epsilon(0.02));
}

// ---- Built-in shaders through the generated GLSL ES (ADR-0144 phase 2) ----------------------

#include "victus/render/builtin_shaders.hpp"

namespace {

// Runs fullscreen.vert + fxaa.frag over a pattern with a diagonal edge on `dev` and reads back.
std::vector<std::uint8_t> fxaa_on(rhi::Device& dev, std::uint32_t size) {
    auto vs = render::create_builtin_shader(dev, "fullscreen.vert");
    auto fs = render::create_builtin_shader(dev, "fxaa.frag");
    REQUIRE_MESSAGE(vs.ok(), (vs.ok() ? "" : vs.error().message));
    REQUIRE_MESSAGE(fs.ok(), (fs.ok() ? "" : fs.error().message));
    rhi::PipelineDesc p;
    p.name = "gles_fxaa";
    p.vertex = vs.value();
    p.fragment = fs.value();
    p.cull = rhi::CullMode::None;
    p.color_targets = {{rhi::Format::RGBA8_UNORM}};
    auto pipe = dev.create_pipeline(p);
    REQUIRE_MESSAGE(pipe.ok(), (pipe.ok() ? "" : pipe.error().message));

    // Bright above the line y = x / 2 + 3, dark below; luma in alpha. Distinct per-row tint so a
    // vertical flip anywhere shows up.
    std::vector<std::uint8_t> src(std::size_t{size} * size * 4);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool bright = static_cast<float>(y) < static_cast<float>(x) * 0.5f + 3.0f;
            const auto v = static_cast<std::uint8_t>(bright ? 230 : 20);
            std::uint8_t* t = src.data() + (std::size_t{y} * size + x) * 4;
            t[0] = v;
            t[1] = static_cast<std::uint8_t>(y * 255 / (size - 1));
            t[2] = static_cast<std::uint8_t>(x * 255 / (size - 1));
            t[3] = v;
        }
    rhi::TextureDesc td;
    td.name = "gles_fxaa_src";
    td.width = td.height = size;
    auto tex = dev.create_texture(td);
    REQUIRE(tex.ok());
    rhi::SamplerDesc sd;
    sd.address_u = sd.address_v = rhi::AddressMode::ClampToEdge;
    auto sampler = dev.create_sampler(sd);
    REQUIRE(sampler.ok());
    rhi::TextureDesc rd;
    rd.name = "gles_fxaa_dst";
    rd.width = rd.height = size;
    rd.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    auto rt = dev.create_texture(rd);
    REQUIRE(rt.ok());

    auto cmd = dev.begin_commands();
    cmd.upload_texture(tex.value(), src);
    const float params[4] = {1.0f / static_cast<float>(size), 1.0f / static_cast<float>(size), 0.0f, 0.75f};
    rhi::RenderPassDesc rp;
    rp.colors = {{rt.value()}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    pass.bind_pipeline(pipe.value());
    cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, params, sizeof params);
    pass.bind_texture(rhi::ShaderStage::Fragment, 0, tex.value(), sampler.value());
    pass.draw(3);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt.value());
    REQUIRE(px.ok());
    auto out = std::move(px).value();
    dev.destroy(pipe.value());
    dev.destroy(vs.value());
    dev.destroy(fs.value());
    dev.destroy(tex.value());
    dev.destroy(sampler.value());
    dev.destroy(rt.value());
    return out;
}

} // namespace

TEST_CASE("gles: every built-in shader with GLSL ES compiles on the device") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    int compiled = 0, without = 0;
    for (const auto& s : render::builtin_shaders()) {
        if (s.stage == rhi::ShaderStage::Compute) continue;
        if (s.glsl_es.empty()) {
            ++without;
            continue;
        }
        auto h = render::create_builtin_shader(dev, s.name);
        CHECK_MESSAGE(h.ok(), s.name << ": " << (h.ok() ? "" : h.error().message));
        if (h.ok()) {
            ++compiled;
            dev.destroy(h.value());
        }
    }
    MESSAGE(compiled << " built-in shaders compiled as GLSL ES 3.00, " << without << " have none yet");
    CHECK(compiled >= 54); // 55 graphics shaders; pbr.frag#fp16 needs shaderFloat16, which GLES never reports (ADR-0144)
}

TEST_CASE("gles: built-in fullscreen pass matches the SDL_GPU backend") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    VICTUS_REQUIRE_GLES_OR_SKIP();
    constexpr std::uint32_t kSize = 24;
    const auto ref = fxaa_on(*test::gpu().device, kSize);
    const auto got = fxaa_on(*gles().device, kSize);
    REQUIRE(ref.size() == got.size());
    int worst = 0;
    double sum = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const int d = std::abs(int(ref[i]) - int(got[i]));
        worst = std::max(worst, d);
        sum += d;
    }
    MESSAGE("fxaa GLES vs " << test::gpu().device->capabilities().driver_name << ": max diff " << worst << ", mean "
                            << sum / static_cast<double>(ref.size()));
    test::save_output("gles_fxaa_ref", kSize, kSize, ref);
    test::save_output("gles_fxaa_gles", kSize, kSize, got);
    CHECK(worst <= 2);
}

namespace {

// fullscreen.vert + sky.frag (procedural gradient, compact-HDR quantisation on) into RGBA16F.
std::vector<std::uint8_t> sky_on(rhi::Device& dev, std::uint32_t w, std::uint32_t h) {
    auto vs = render::create_builtin_shader(dev, "fullscreen.vert");
    auto fs = render::create_builtin_shader(dev, "sky.frag");
    REQUIRE_MESSAGE(fs.ok(), (fs.ok() ? "" : fs.error().message));
    REQUIRE(vs.ok());
    rhi::PipelineDesc p;
    p.name = "gles_sky";
    p.vertex = vs.value();
    p.fragment = fs.value();
    p.cull = rhi::CullMode::None;
    p.color_targets = {{rhi::Format::RGBA16_FLOAT}};
    auto pipe = dev.create_pipeline(p);
    REQUIRE_MESSAGE(pipe.ok(), (pipe.ok() ? "" : pipe.error().message));
    rhi::TextureDesc cd;
    cd.name = "gles_sky_env";
    cd.type = rhi::TextureType::Cube;
    cd.depth_or_layers = 6;
    auto cube = dev.create_texture(cd);
    REQUIRE(cube.ok());
    auto sampler = dev.create_sampler({});
    REQUIRE(sampler.ok());
    rhi::TextureDesc rd;
    rd.name = "gles_sky_dst";
    rd.format = rhi::Format::RGBA16_FLOAT;
    rd.width = w;
    rd.height = h;
    rd.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    auto rt = dev.create_texture(rd);
    REQUIRE(rt.ok());
    struct {
        float inv_view_proj[16];
        float zenith[4], horizon[4], ground[4], sun_dir[4], sun_color[4], env[4];
    } u{};
    // Identity view-projection: rays run along +z with y spanning the view, so the image covers
    // ground, horizon, zenith and the sun.
    for (int i = 0; i < 4; ++i) u.inv_view_proj[i * 5] = 1.0f;
    const float zenith[4] = {0.2f, 0.45f, 1.6f, 0}, horizon[4] = {1.1f, 1.3f, 1.7f, 0}, ground[4] = {0.3f, 0.25f, 0.2f, 0};
    const float sun_dir[4] = {0.3f, 0.4f, 1.0f, 3.0f}, sun_color[4] = {40.0f, 36.0f, 30.0f, 0.995f};
    std::memcpy(u.zenith, zenith, sizeof zenith);
    std::memcpy(u.horizon, horizon, sizeof horizon);
    std::memcpy(u.ground, ground, sizeof ground);
    std::memcpy(u.sun_dir, sun_dir, sizeof sun_dir);
    std::memcpy(u.sun_color, sun_color, sizeof sun_color);
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt.value()}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    pass.bind_pipeline(pipe.value());
    cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, &u, sizeof u);
    pass.bind_texture(rhi::ShaderStage::Fragment, 0, cube.value(), sampler.value());
    pass.draw(3);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt.value());
    REQUIRE(px.ok());
    auto out = std::move(px).value();
    dev.destroy(pipe.value());
    dev.destroy(vs.value());
    dev.destroy(fs.value());
    dev.destroy(cube.value());
    dev.destroy(sampler.value());
    dev.destroy(rt.value());
    return out;
}

float half_to_float(std::uint16_t h) {
    const std::uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff;
    const float mag = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    return (h & 0x8000) ? -mag : mag;
}

} // namespace

TEST_CASE("gles: sky with compact-HDR rounding matches the SDL_GPU backend") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    VICTUS_REQUIRE_GLES_OR_SKIP();
    if (!gles().device->supports_format(rhi::Format::RGBA16_FLOAT, rhi::TextureUsageColorTarget)) return;
    constexpr std::uint32_t kW = 32, kH = 32;
    const auto ref = sky_on(*test::gpu().device, kW, kH);
    const auto got = sky_on(*gles().device, kW, kH);
    REQUIRE(ref.size() == got.size());
    double worst = 0;
    int unquantised = 0, exact = 0;
    for (std::size_t i = 0; i < ref.size() / 2; ++i) {
        if (i % 4 == 3) continue; // alpha
        const std::uint16_t a = half_bits(ref, i), b = half_bits(got, i);
        exact += a == b;
        const float fa = half_to_float(a), fb = half_to_float(b);
        worst = std::max(worst, static_cast<double>(std::abs(fa - fb) / std::max(fa, 1e-3f)));
        // R11G11B10: 6 mantissa bits for red/green, 5 for blue. Normal halves keep 10.
        const std::uint32_t drop = i % 4 == 2 ? 5 : 4;
        if (((b >> 10) & 0x1f) != 0 && (b & ((1u << drop) - 1)) != 0) ++unquantised;
    }
    const int channels = static_cast<int>(kW * kH * 3);
    MESSAGE("sky GLES vs SDL_GPU: " << exact << "/" << channels << " channels identical, worst relative diff " << worst);
    CHECK(unquantised == 0);
    CHECK(worst < 0.04); // one R11G11B10 step is 1/64 relative; the noise may round either way
}

TEST_CASE("gles: skinned vertices read the joint palette from a storage buffer") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    auto& dev = *gles().device;
    auto vs = render::create_builtin_shader(dev, "mesh_skinned.vert");
    REQUIRE_MESSAGE(vs.ok(), (vs.ok() ? "" : vs.error().message));
    rhi::ShaderDesc fd;
    fd.name = "gles_skin_fs";
    fd.stage = rhi::ShaderStage::Fragment;
    fd.glsl_es = "#version 300 es\nprecision highp float;\nin vec3 vx_io_0;\nlayout(location = 0) out vec4 o;\n"
                 "void main() { o = vec4(1.0, 0.0, vx_io_0.z, 1.0); }\n";
    auto fs = dev.create_shader(fd);
    REQUIRE_MESSAGE(fs.ok(), (fs.ok() ? "" : fs.error().message));
    rhi::PipelineDesc p;
    p.name = "gles_skin";
    p.vertex = vs.value();
    p.fragment = fs.value();
    p.cull = rhi::CullMode::None;
    p.vertex_buffers = {{0, 48, false}, {1, 112, true}, {2, 28, false}};
    p.attributes = {{0, 0, rhi::VertexFormat::Float3, 0},    {1, 0, rhi::VertexFormat::Float3, 12},
                    {2, 0, rhi::VertexFormat::Float2, 24},   {3, 0, rhi::VertexFormat::Float4, 32},
                    {4, 1, rhi::VertexFormat::Float4, 0},    {5, 1, rhi::VertexFormat::Float4, 16},
                    {6, 1, rhi::VertexFormat::Float4, 32},   {7, 1, rhi::VertexFormat::Float4, 48},
                    {8, 1, rhi::VertexFormat::Float4, 64},   {9, 1, rhi::VertexFormat::Float4, 80},
                    {10, 1, rhi::VertexFormat::Float4, 96},  {11, 2, rhi::VertexFormat::UShort4, 0},
                    {12, 2, rhi::VertexFormat::Float4, 8},   {13, 2, rhi::VertexFormat::UByte4Norm, 24}};
    p.color_targets = {{rhi::Format::RGBA8_UNORM}};
    auto pipe = dev.create_pipeline(p);
    REQUIRE_MESSAGE(pipe.ok(), (pipe.ok() ? "" : pipe.error().message));

    // Quad over the left half of clip space; the skin moves it to the right half.
    std::vector<float> verts;
    for (auto [x, y] : {std::pair{-1.0f, -1.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}, {-1.0f, 1.0f}})
        verts.insert(verts.end(), {x, y, 0.5f, 0, 0, 1, 0, 0, 1, 0, 0, 1});
    struct Skin {
        std::uint16_t joints[4];
        float weights[4];
        std::uint8_t color[4];
    };
    static_assert(sizeof(Skin) == 28);
    const std::vector<Skin> skin(4, Skin{{1, 0, 0, 0}, {1, 0, 0, 0}, {255, 255, 255, 255}});
    // Identity model and normal matrices; normal0.w is the palette base (300 matrices = texel
    // 1200, in the data texture's second row).
    const std::vector<float> inst = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 300, 0, 1, 0, 0, 0, 0, 1, 0};
    std::vector<float> palette(400 * 16, 0.0f);
    auto translate = [&](std::size_t m, float tx) {
        float* a = palette.data() + m * 16;
        a[0] = a[5] = a[10] = a[15] = 1.0f;
        a[12] = tx;
    };
    translate(300, -5.0f); // joint 0 of the instance: unused, off screen if indexed by mistake
    translate(301, 1.0f);  // joint 1
    auto vb = buffer(verts, rhi::BufferUsageVertex);
    auto ib = buffer(kQuadIndices, rhi::BufferUsageIndex);
    auto ibuf = buffer(inst, rhi::BufferUsageVertex);
    auto sb = buffer(skin, rhi::BufferUsageVertex);
    auto pal = buffer(palette, rhi::BufferUsageStorageRead);
    const auto rt = target(4, 2, rhi::Format::RGBA8_UNORM);
    float view_proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto cmd = dev.begin_commands();
    rhi::RenderPassDesc rp;
    rp.colors = {{rt}};
    auto pass = cmd.begin_render_pass(rp);
    REQUIRE(pass.active());
    pass.bind_pipeline(pipe.value());
    cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj, sizeof view_proj);
    pass.bind_vertex_buffer(0, vb);
    pass.bind_vertex_buffer(1, ibuf);
    pass.bind_vertex_buffer(2, sb);
    pass.bind_index_buffer(ib, rhi::IndexType::U16);
    pass.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, pal);
    pass.draw_indexed(6, 1, 0, 0, 0);
    pass.end();
    cmd.submit_and_wait();
    auto px = dev.read_texture(rt);
    REQUIRE(px.ok());
    for (std::uint32_t y = 0; y < 2; ++y) {
        CHECK(test::pixel(px.value(), 4, 0, y) == 0xff000000u);
        CHECK(test::pixel(px.value(), 4, 1, y) == 0xff000000u);
        CHECK(test::channel(test::pixel(px.value(), 4, 2, y), 0) == 255);
        CHECK(test::channel(test::pixel(px.value(), 4, 3, y), 0) == 255);
    }
    // A partial upload into the second row moves the quad back.
    translate(301, 0.0f);
    auto up = dev.begin_commands();
    up.upload_buffer(pal, 301 * 64, {reinterpret_cast<const std::uint8_t*>(palette.data() + 301 * 16), 64});
    up.submit_and_wait();
    auto cmd2 = dev.begin_commands();
    auto pass2 = cmd2.begin_render_pass(rp);
    pass2.bind_pipeline(pipe.value());
    cmd2.push_uniforms(rhi::ShaderStage::Vertex, 0, view_proj, sizeof view_proj);
    pass2.bind_vertex_buffer(0, vb);
    pass2.bind_vertex_buffer(1, ibuf);
    pass2.bind_vertex_buffer(2, sb);
    pass2.bind_index_buffer(ib, rhi::IndexType::U16);
    pass2.bind_storage_buffer(rhi::ShaderStage::Vertex, 0, pal);
    pass2.draw_indexed(6, 1, 0, 0, 0);
    pass2.end();
    cmd2.submit_and_wait();
    auto px2 = dev.read_texture(rt);
    REQUIRE(px2.ok());
    CHECK(test::channel(test::pixel(px2.value(), 4, 0, 0), 0) == 255);
    CHECK(test::pixel(px2.value(), 4, 3, 0) == 0xff000000u);
}

namespace {

// One Low-tier frame of a lit cube on a plane, rendered by the scene renderer on CTX.
std::vector<std::uint8_t> render_low_scene(render::GpuContext& ctx, std::uint32_t w, std::uint32_t h, std::string& error) {
    auto& dev = ctx.device();
    auto sr = render::SceneRenderer::create(ctx);
    if (!sr) {
        error = "scene renderer: " + sr.error().message;
        return {};
    }
    auto cube = ctx.create_mesh(render::make_cube(), "gles_cube").value();
    auto plane = ctx.create_mesh(render::make_plane({200, 200}), "gles_plane").value();
    // A skinned column bent at its middle joint: the vertex stage reads the joint palette from a
    // storage slot while pbr.frag reads its light lists from its own (distinct GL uniforms).
    const assets::CookedModel model = test::make_skinned_column();
    auto column = ctx.create_mesh(model.meshes[0].data, "gles_column").value();
    const math::Mat4 palette[2] = {math::Mat4::identity(),
                                   math::Mat4::translation({0, 1, 0}) *
                                       math::Mat4::rotation(math::Quat::axis_angle({0, 0, 1}, 1.2f)) *
                                       math::Mat4::translation({0, -1, 0})};
    render::Material red;
    red.base_color = {0.9f, 0.1f, 0.05f, 1};
    rhi::TextureDesc td;
    td.name = "gles_scene_target";
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = w;
    td.height = h;
    td.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    auto target = dev.create_texture(td).value();
    sr.value()->set_settings(render::RenderSettings::preset(render::QualityTier::Low));
    const render::Camera3D cam = render::Camera3D::look_at({0, 4, 8}, {0, 0.5f, 0});
    std::vector<std::uint8_t> px;
    sr.value()->begin(cam, render::DirectionalLight{}, render::Environment{});
    sr.value()->submit(plane, {}, math::Mat4::identity());
    sr.value()->submit(cube, {}, math::Mat4::translation({0, 1, 0}));
    sr.value()->submit_skinned(column, red, math::Mat4::translation({2.5f, 0, 0}), palette,
                               math::Aabb{{1.2f, -0.3f, -0.3f}, {2.8f, 1.3f, 0.3f}});
    auto cmd = dev.begin_commands();
    auto st = sr.value()->render(cmd, target, w, h, rhi::Format::RGBA8_UNORM);
    if (!st) error = "render: " + st.error().message;
    cmd.submit_and_wait();
    if (st) {
        auto r = dev.read_texture(target);
        if (r) px = std::move(r).value();
        else error = "readback: " + r.error().message;
    }
    dev.destroy(target);
    ctx.destroy(cube);
    ctx.destroy(plane);
    ctx.destroy(column);
    return px;
}

} // namespace

TEST_CASE("gles: the scene renderer draws the Low tier like Vulkan") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    VICTUS_REQUIRE_GPU_OR_SKIP();
    constexpr std::uint32_t W = 192, H = 128;
    auto c = render::GpuContext::create(*gles().device);
    REQUIRE_MESSAGE(c.ok(), (c.ok() ? "" : c.error().message));
    std::string err;
    const auto got = render_low_scene(*c.value(), W, H, err);
    REQUIRE_MESSAGE(err.empty(), err);
    const auto ref = render_low_scene(*test::gpu().ctx, W, H, err);
    REQUIRE_MESSAGE(err.empty(), err);
    test::save_output("gles_scene_low_gles", W, H, got);
    test::save_output("gles_scene_low_ref", W, H, ref);
    REQUIRE(got.size() == ref.size());
    int worst = 0;
    double sum = 0;
    std::size_t over8 = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (i % 4 == 3) continue;
        const int d = std::abs(int(got[i]) - int(ref[i]));
        worst = std::max(worst, d);
        sum += d;
        over8 += d > 8 ? 1 : 0;
    }
    const double mean = sum / (double(got.size()) * 0.75);
    MESSAGE("scene Low GLES vs " << test::gpu().device->capabilities().driver_name << ": max diff " << worst << ", mean "
                                 << mean << ", channels > 8: " << over8 << "/" << got.size() / 4 * 3);
    // Same rasteriser on CI (Mesa), different API: allow rounding, not structural differences.
    CHECK(mean < 1.0);
    CHECK(over8 * 100 <= got.size() / 4 * 3);
    // The bent skinned column is on screen (reddish pixels), not collapsed by a wrong palette.
    std::size_t red_px = 0;
    for (std::size_t i = 0; i + 3 < got.size(); i += 4) red_px += (got[i] > 120 && got[i] > got[i + 1] + 60) ? 1 : 0;
    CHECK(red_px > 100);
}

// ADR-0144 phase 3: the player asks for the SDL_GPU backends with gles_fallback, then creates its
// window with SDL_WINDOW_OPENGL when the device came back on GLES, and presents through it.
TEST_CASE("gles: device creation falls back to GLES and presents to an OpenGL window") {
    VICTUS_REQUIRE_GLES_OR_SKIP();
    rhi::DeviceDesc dd;
    dd.headless = true;
    dd.preferred_driver = "victus-no-such-driver"; // no SDL_GPU backend has this name
    {
        auto strict = rhi::Device::create(dd);
        CHECK_FALSE(strict.ok());
    }
    dd.gles_fallback = true;
    auto created = rhi::Device::create(dd);
    REQUIRE_MESSAGE(created.ok(), (created.ok() ? std::string{} : created.error().message));
    auto& dev = **created;
    CHECK(dev.capabilities().backend == rhi::Backend::GLES);
    CHECK_FALSE(dev.capabilities().compute);

    platform::WindowDesc wd;
    wd.title = "gles present";
    wd.width = 64;
    wd.height = 32;
    wd.hidden = true;
    wd.high_dpi = false;
    {
        auto plain = test::gpu().platform->create_window(wd);
        REQUIRE(plain.ok());
        CHECK_FALSE(dev.attach_window(**plain).ok()); // no SDL_WINDOW_OPENGL
    }
    wd.opengl = true;
    auto window = test::gpu().platform->create_window(wd);
    REQUIRE(window.ok());
    REQUIRE(dev.attach_window(**window, rhi::PresentMode::Immediate).ok());
    rhi::TextureHandle last;
    for (int frame = 0; frame < 3; ++frame) {
        auto cmd = dev.begin_commands();
        std::uint32_t w = 0, h = 0;
        const auto sc = cmd.acquire_swapchain(**window, w, h);
        REQUIRE(sc.valid());
        CHECK(w == 64);
        CHECK(h == 32);
        rhi::RenderPassDesc rp;
        rp.colors = {{sc, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 1, 0, 1}}};
        auto pass = cmd.begin_render_pass(rp);
        REQUIRE(pass.active());
        pass.end();
        cmd.submit_and_wait();
        last = sc;
    }
    auto px = dev.read_texture(last);
    REQUIRE(px.ok());
    CHECK(test::pixel(px.value(), 64, 0, 0) == 0xff00ff00u);
    CHECK(test::pixel(px.value(), 64, 63, 31) == 0xff00ff00u);
    CHECK(dev.stats().frames_submitted >= 3);
    dev.detach_window(**window);
}
