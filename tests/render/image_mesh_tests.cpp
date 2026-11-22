// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/render/image.hpp"
#include "victus/render/mesh.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;

TEST_CASE("png encode/decode round trip is lossless") {
    Image img = make_checker(16, 4, 0xFF0000FFu, 0x80FF8000u);
    auto png = encode_png(img);
    REQUIRE(png.ok());
    CHECK(png->size() > 8);
    auto back = decode_image(png.value());
    REQUIRE(back.ok());
    CHECK(back->width == 16);
    CHECK(back->height == 16);
    CHECK(back->pixels == img.pixels);
    CHECK(back->pixel(0, 0) == 0xFF0000FFu);
    CHECK(back->pixel(4, 0) == 0x80FF8000u);
}

TEST_CASE("decode rejects garbage") {
    const std::uint8_t junk[] = {1, 2, 3, 4, 5};
    CHECK_FALSE(decode_image(junk).ok());
}

TEST_CASE("mip chain sizes and box filtering") {
    Image img = make_checker(8, 8, 0xFFFFFFFFu, 0xFF000000u); // 1-pixel checker
    auto chain = build_mip_chain(img, false);
    REQUIRE(chain.size() == 4);
    CHECK(mip_count(8, 8) == 4);
    CHECK(mip_count(300, 7) == 9);
    CHECK(chain[3].width == 1);
    // Linear average of black/white is mid grey.
    const std::uint8_t v = chain[1].pixels[0];
    CHECK(v >= 127);
    CHECK(v <= 128);
    // sRGB-aware filtering averages in linear light => brighter encoded value (~188).
    auto srgb = build_mip_chain(img, true);
    CHECK(srgb[1].pixels[0] > 180);
    CHECK(srgb[1].pixels[3] == 255);
}

TEST_CASE("coverage-preserving alpha mips keep the alpha-test coverage") {
    // 1-texel opaque speckles on a transparent background (~25% coverage): box filtering drives
    // every coarser texel below the 0.5 cutoff.
    Image img;
    img.width = img.height = 64;
    img.pixels.assign(64 * 64 * 4, 255);
    for (std::uint32_t i = 0; i < 64 * 64; ++i) {
        std::uint32_t h = i; // lowbias32 integer hash
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        img.pixels[i * 4 + 3] = h % 4 == 0 ? 255 : 0;
    }
    const float target = alpha_coverage(img, 0.5f);
    CHECK(target > 0.2f);
    CHECK(target < 0.3f);
    auto plain = build_mip_chain(img, false);
    auto kept = plain;
    preserve_alpha_coverage(kept, 0.5f);
    REQUIRE(kept.size() == 7);
    CHECK(kept[0].pixels == plain[0].pixels); // the source level is untouched
    CHECK(alpha_coverage(plain[2], 0.5f) < 0.05f);
    for (std::size_t m = 1; m < 5; ++m) { // levels with at least 16 texels
        INFO("mip " << m);
        CHECK(std::abs(alpha_coverage(kept[m], 0.5f) - target) < 0.1f);
        for (std::size_t i = 0; i < kept[m].pixels.size(); i += 4) // colour is not touched
            CHECK(kept[m].pixels[i] == plain[m].pixels[i]);
    }
    // Disabled cutoffs and single-level chains are no-ops.
    auto same = plain;
    preserve_alpha_coverage(same, 0.0f);
    preserve_alpha_coverage(same, 1.0f);
    CHECK(same[3].pixels == plain[3].pixels);
    std::vector<Image> one{img};
    preserve_alpha_coverage(one, 0.5f);
    CHECK(one[0].pixels == img.pixels);
    CHECK(alpha_coverage(Image{}, 0.5f) == 0.0f);
}

TEST_CASE("procedural meshes are well formed") {
    for (const MeshData& m : {make_cube(), make_uv_sphere(1.0f, 24, 12), make_plane({4, 4}, 4), make_cylinder()}) {
        REQUIRE(!m.vertices.empty());
        REQUIRE(m.indices.size() % 3 == 0);
        for (auto i : m.indices) REQUIRE(i < m.vertices.size());
        for (const auto& v : m.vertices) {
            CHECK(std::fabs(math::length(v.normal) - 1.0f) < 1e-3f);
            CHECK(std::fabs(math::dot(v.normal, v.tangent.xyz())) < 1e-3f);
            CHECK(std::fabs(v.tangent.w) == doctest::Approx(1.0f));
        }
    }
    MeshData cube = make_cube({1, 2, 3});
    CHECK(cube.vertices.size() == 24);
    CHECK(cube.triangle_count() == 12);
    auto b = cube.bounds();
    CHECK(b.max.y == doctest::Approx(2.0f));
    CHECK(b.min.z == doctest::Approx(-3.0f));
    // Triangles wind counter-clockwise when seen from outside.
    for (std::size_t i = 0; i < cube.indices.size(); i += 3) {
        const auto& a = cube.vertices[cube.indices[i]];
        const auto& c1 = cube.vertices[cube.indices[i + 1]];
        const auto& c2 = cube.vertices[cube.indices[i + 2]];
        math::Vec3 n = math::cross(c1.position - a.position, c2.position - a.position);
        CHECK(math::dot(n, a.normal) > 0.0f);
    }
    MeshData sphere = make_uv_sphere(2.0f);
    CHECK(sphere.bounds().max.x == doctest::Approx(2.0f).epsilon(0.01));
}

TEST_CASE("mesh append transforms geometry") {
    MeshData a = make_cube();
    MeshData merged;
    merged.append(a, math::Mat4::translation({10, 0, 0}));
    merged.append(a, math::Mat4::identity());
    CHECK(merged.vertices.size() == 48);
    CHECK(merged.indices.size() == 72);
    CHECK(merged.bounds().max.x == doctest::Approx(10.5f));
    CHECK(merged.indices[36] == 24);
}

TEST_CASE("make_skin_vertex merges duplicate joints, keeps the four largest and reports what changed") {
    SkinWeightStats st;
    {
        const std::uint32_t j[3] = {2, 5, 2};
        const float w[3] = {0.25f, 0.5f, 0.25f};
        const auto v = make_skin_vertex(j, w, 3, &st);
        CHECK(v.joints[0] == 2); // the merged 0.5 is listed first and ties joint 5
        CHECK(v.joints[1] == 5);
        CHECK(v.weights[0] + v.weights[1] == doctest::Approx(1.0f));
        CHECK(v.weights[2] == 0.0f);
    }
    CHECK(st.duplicate_joints == 1);
    CHECK(st.over_limit == 0);
    {
        const std::uint32_t j[6] = {0, 1, 2, 3, 4, 5};
        const float w[6] = {0.3f, 0.25f, 0.2f, 0.15f, 0.06f, 0.04f};
        const auto v = make_skin_vertex(j, w, 6, &st);
        CHECK(v.joints[0] == 0);
        CHECK(v.joints[3] == 3);
        CHECK(v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3] == doctest::Approx(1.0f));
    }
    CHECK(st.over_limit == 1);
    CHECK(st.max_dropped_weight == doctest::Approx(0.06f).epsilon(1e-3));
    {
        const std::uint32_t j[2] = {7, 8};
        const float w[2] = {0, 0};
        const auto v = make_skin_vertex(j, w, 2, &st);
        CHECK(v.joints[0] == 0);
        CHECK(v.weights[0] == 1.0f);
    }
    CHECK(st.unweighted == 1);
    CHECK(st.vertices == 3);
}
