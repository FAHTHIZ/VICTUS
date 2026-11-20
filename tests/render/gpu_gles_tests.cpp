// SPDX-License-Identifier: Apache-2.0
// OpenGL ES 3.0 backend (ADR-0144 phase 1): draws through the public RHI on a GLES device and
// reads the pixels back. The shaders are hand-written GLSL ES 3.00 that follow the backend's
// conventions (clip space fixed up as spirv-cross --fixup-clipspace --flip-vert-y does, slots
// declared with "//!" comments), so the expected images match what the SDL_GPU backends produce
// from the equivalent SPIR-V.
#include "gpu_fixture.hpp"

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
