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
