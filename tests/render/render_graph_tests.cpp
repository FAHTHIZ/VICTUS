// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/render/render_graph.hpp"

using namespace victus;
using namespace victus::render;

namespace {
rhi::TextureDesc color_desc(std::uint32_t w = 64, std::uint32_t h = 64) {
    rhi::TextureDesc d;
    d.format = rhi::Format::RGBA16_FLOAT;
    d.width = w;
    d.height = h;
    d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    return d;
}
} // namespace

TEST_CASE("render graph culls passes that do not reach an output") {
    RenderGraph g;
    auto out = g.import_texture("backbuffer", {1, 1}, color_desc());
    auto a = g.create_texture("a", color_desc());
    auto unused = g.create_texture("unused", color_desc());
    g.add_pass("produce_a", [&](PassBuilder& b) { b.write_color(a); }, nullptr);
    g.add_pass("dead", [&](PassBuilder& b) { b.write_color(unused); }, nullptr);
    g.add_pass("compose", [&](PassBuilder& b) {
        b.read(a);
        b.write_color(out);
    }, nullptr);
    g.mark_output(out);
    REQUIRE(g.compile().ok());
    const auto& info = g.info();
    REQUIRE(info.executed_passes.size() == 2);
    CHECK(info.executed_passes[0] == "produce_a");
    CHECK(info.executed_passes[1] == "compose");
    CHECK(info.culled_passes == 1);
    CHECK(info.transient_textures == 1);
}

TEST_CASE("render graph keeps side-effect passes and load dependencies") {
    RenderGraph g;
    auto out = g.import_texture("out", {1, 1}, color_desc());
    auto t = g.create_texture("t", color_desc());
    g.add_pass("clear_t", [&](PassBuilder& b) { b.write_color(t); }, nullptr);
    g.add_pass("accumulate_t", [&](PassBuilder& b) { b.write_color(t, rhi::LoadOp::Load); }, nullptr);
    g.add_pass("readback", [&](PassBuilder& b) {
        b.read(t);
        b.no_raster();
        b.side_effect();
    }, nullptr);
    g.add_pass("unrelated_output", [&](PassBuilder& b) { b.write_color(out); }, nullptr);
    REQUIRE(g.compile().ok());
    CHECK(g.info().executed_passes.size() == 3); // out is not marked as output: its writer is culled
    CHECK(g.info().culled_passes == 1);
}

TEST_CASE("render graph aliases transients with disjoint lifetimes") {
    RenderGraph g;
    auto out = g.import_texture("out", {1, 1}, color_desc());
    auto a = g.create_texture("a", color_desc());
    auto b = g.create_texture("b", color_desc());
    auto c = g.create_texture("c", color_desc());
    auto small = g.create_texture("small", color_desc(32, 32));
    g.add_pass("p0", [&](PassBuilder& p) { p.write_color(a); }, nullptr);
    g.add_pass("p1", [&](PassBuilder& p) { p.read(a); p.write_color(b); }, nullptr);
    g.add_pass("p2", [&](PassBuilder& p) { p.read(b); p.write_color(c); }, nullptr); // a is dead here
    g.add_pass("p3", [&](PassBuilder& p) { p.read(c); p.write_color(small); }, nullptr);
    g.add_pass("p4", [&](PassBuilder& p) { p.read(small); p.write_color(out); }, nullptr);
    g.mark_output(out);
    REQUIRE(g.compile().ok());
    CHECK(g.info().transient_textures == 4);
    CHECK(g.alias_slot(c) == g.alias_slot(a));      // a ends at p1, c begins at p2
    CHECK(g.alias_slot(b) != g.alias_slot(a));
    CHECK(g.alias_slot(small) != g.alias_slot(a)); // different size never aliases
    CHECK(g.info().physical_textures == 3);
    CHECK(g.alias_slot(out) == -1);
}

TEST_CASE("render graph validates misuse") {
    SUBCASE("reading a transient before writing it") {
        RenderGraph g;
        auto t = g.create_texture("t", color_desc());
        auto out = g.import_texture("out", {1, 1}, color_desc());
        g.add_pass("bad", [&](PassBuilder& b) { b.read(t); b.write_color(out); }, nullptr);
        g.mark_output(out);
        CHECK_FALSE(g.compile().ok());
    }
    SUBCASE("colour write into a depth texture") {
        RenderGraph g;
        auto d = color_desc();
        d.format = rhi::Format::D32_FLOAT;
        auto t = g.create_texture("depth", d);
        g.add_pass("bad", [&](PassBuilder& b) { b.write_color(t); }, nullptr);
        g.mark_output(t);
        CHECK_FALSE(g.compile().ok());
    }
    SUBCASE("raster pass without attachments") {
        RenderGraph g;
        g.add_pass("empty", [&](PassBuilder& b) { b.side_effect(); }, nullptr);
        CHECK_FALSE(g.compile().ok());
    }
}

TEST_CASE("render graph resolves multisampled colour into a single-sample texture (ADR-0152)") {
    rhi::TextureDesc ms = color_desc();
    ms.sample_count = 4;
    ms.usage = rhi::TextureUsageColorTarget;
    rhi::TextureDesc ms_depth = ms;
    ms_depth.format = rhi::Format::D32_FLOAT;
    ms_depth.usage = rhi::TextureUsageDepthTarget;
    SUBCASE("the resolve target is written by the pass and feeds later readers") {
        RenderGraph g;
        auto out = g.import_texture("out", {1, 1}, color_desc());
        auto msaa = g.create_texture("msaa", ms);
        auto depth = g.create_texture("depth_msaa", ms_depth);
        auto hdr = g.create_texture("hdr", color_desc());
        g.add_pass("forward", [&](PassBuilder& b) {
            b.write_color_resolved(msaa, hdr);
            b.write_depth(depth, rhi::LoadOp::Clear, 1.0f, false);
        }, nullptr);
        g.add_pass("compose", [&](PassBuilder& b) {
            b.read(hdr);
            b.write_color(out);
        }, nullptr);
        g.mark_output(out);
        REQUIRE(g.compile().ok());
        REQUIRE(g.info().executed_passes.size() == 2);
        CHECK(g.info().executed_passes[0] == "forward");
        CHECK(g.alias_slot(msaa) != g.alias_slot(hdr)); // different sample counts never alias
    }
    SUBCASE("mismatched resolve targets are rejected") {
        RenderGraph g;
        auto out = g.import_texture("out", {1, 1}, color_desc());
        auto msaa = g.create_texture("msaa", ms);
        auto small = g.create_texture("small", color_desc(32, 32));
        g.add_pass("forward", [&](PassBuilder& b) { b.write_color_resolved(msaa, small); }, nullptr);
        g.add_pass("compose", [&](PassBuilder& b) {
            b.read(small);
            b.write_color(out);
        }, nullptr);
        g.mark_output(out);
        CHECK_FALSE(g.compile().ok());
    }
    SUBCASE("a single-sample source cannot resolve") {
        RenderGraph g;
        auto out = g.import_texture("out", {1, 1}, color_desc());
        auto one = g.create_texture("one", color_desc());
        auto hdr = g.create_texture("hdr", color_desc());
        g.add_pass("forward", [&](PassBuilder& b) { b.write_color_resolved(one, hdr); }, nullptr);
        g.add_pass("compose", [&](PassBuilder& b) {
            b.read(hdr);
            b.write_color(out);
        }, nullptr);
        g.mark_output(out);
        CHECK_FALSE(g.compile().ok());
    }
}
