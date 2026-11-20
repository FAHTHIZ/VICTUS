// SPDX-License-Identifier: Apache-2.0
// Baked lighting (ADR-0122): lightmap unwrap, the CPU path tracer's physical units (furnace and
// direct-light tests against closed-form answers), contact darkening, colour bleeding, leak
// rejection, thread-count independence and the .vlightmap file format.
#include <algorithm>
#include "doctest.h"
#include "victus/core/jobs.hpp"
#include "victus/render/lightmap.hpp"

#include <cmath>
#include <optional>

using namespace victus;
using namespace victus::render;
using math::Vec2;
using math::Vec3;

namespace {

LightmapSettings fast_settings() {
    LightmapSettings s;
    s.texels_per_unit = 4.0f;
    s.samples = 96;
    s.bounces = 2;
    return s;
}

// Lightmap uv of a world-space point on a receiving instance (nullopt when off the mesh).
std::optional<Vec2> lightmap_uv_at(const LightmapScene& scene, const LightmapBake& bake, std::size_t instance, Vec3 point) {
    const LightmapInstance& inst = scene.instances[instance];
    const LightmapUnwrap& u = bake.instances[instance];
    for (std::size_t i = 0; i + 2 < u.indices.size(); i += 3) {
        Vec3 p[3];
        Vec2 uv[3];
        for (int j = 0; j < 3; ++j) {
            const std::uint32_t k = u.indices[i + static_cast<std::size_t>(j)];
            p[j] = inst.model.transform_point(inst.mesh->vertices[u.xref[k]].position);
            uv[j] = u.uv[k];
        }
        const Vec3 n = math::cross(p[1] - p[0], p[2] - p[0]);
        const float area2 = math::dot(n, n);
        if (area2 <= 0) continue;
        if (std::fabs(math::dot(point - p[0], math::normalize(n))) > 1e-3f) continue;
        const float w1 = math::dot(math::cross(point - p[0], p[2] - p[0]), n) / area2;
        const float w2 = math::dot(math::cross(p[1] - p[0], point - p[0]), n) / area2;
        const float w0 = 1.0f - w1 - w2;
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
        return uv[0] * w0 + uv[1] * w1 + uv[2] * w2;
    }
    return std::nullopt;
}

std::optional<Vec3> lightmap_at(const LightmapScene& scene, const LightmapBake& bake, std::size_t instance, Vec3 point) {
    const auto t = lightmap_uv_at(scene, bake, instance, point);
    if (!t) return std::nullopt;
    const math::Vec4 v = sample_lightmap(bake.atlas, *t);
    return Vec3{v.x, v.y, v.z};
}

// Directional texel (RGBA8 normalised to 0..1, nearest) at a world-space point.
std::optional<math::Vec4> direction_at(const LightmapScene& scene, const LightmapBake& bake, std::size_t instance, Vec3 point) {
    const auto t = lightmap_uv_at(scene, bake, instance, point);
    if (!t || bake.direction.empty()) return std::nullopt;
    const Image& img = bake.direction;
    const auto x = std::min(static_cast<std::uint32_t>(t->x * static_cast<float>(img.width)), img.width - 1);
    const auto y = std::min(static_cast<std::uint32_t>(t->y * static_cast<float>(img.height)), img.height - 1);
    const std::uint8_t* px = img.pixels.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
    return math::Vec4{px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f, px[3] / 255.0f};
}

Vec3 decode_direction(math::Vec4 t) { return {t.x * 2.0f - 1.0f, t.y * 2.0f - 1.0f, t.z * 2.0f - 1.0f}; }

LightmapSky uniform_sky(float v) {
    LightmapSky sky;
    sky.sky = sky.horizon = sky.ground = Vec3{v};
    return sky;
}

} // namespace

TEST_CASE("lightmap unwrap: valid, in-range and deterministic") {
    const MeshData plane = make_plane({4, 4}, 2);
    const MeshData cube = make_cube();
    LightmapScene scene;
    scene.instances.push_back({&plane});
    scene.instances.push_back({&cube, math::Mat4::translation({0, 0.5f, 0})});
    LightmapInstance occluder{&cube, math::Mat4::translation({3, 0.5f, 0})};
    occluder.receives = false;
    scene.instances.push_back(occluder);

    auto a = unwrap_lightmaps(scene, fast_settings());
    REQUIRE(a);
    auto b = unwrap_lightmaps(scene, fast_settings());
    REQUIRE(b);
    CHECK(a.value().width > 0);
    CHECK(a.value().height > 0);
    REQUIRE(a.value().instances.size() == 3);
    CHECK(a.value().instances[2].empty());
    for (std::size_t i = 0; i < 2; ++i) {
        const LightmapUnwrap& u = a.value().instances[i];
        const MeshData& m = *scene.instances[i].mesh;
        REQUIRE_FALSE(u.empty());
        CHECK(u.indices.size() == m.indices.size());
        CHECK(u.xref.size() == u.uv.size());
        for (std::uint32_t x : u.xref) CHECK(x < m.vertices.size());
        for (std::uint32_t idx : u.indices) CHECK(idx < u.xref.size());
        for (Vec2 uv : u.uv) {
            CHECK(uv.x >= 0.0f);
            CHECK(uv.x <= 1.0f);
            CHECK(uv.y >= 0.0f);
            CHECK(uv.y <= 1.0f);
        }
        CHECK(u.uv == b.value().instances[i].uv);
        CHECK(u.indices == b.value().instances[i].indices);
        // The re-indexed mesh draws the same triangles (positions through xref).
        const MeshData re = apply_lightmap_unwrap(m, u);
        for (std::size_t t = 0; t < re.indices.size(); ++t) CHECK(re.vertices[re.indices[t]].position == m.vertices[m.indices[t]].position);
    }
}

TEST_CASE("lightmap unwrap lowers the density when the scene does not fit") {
    const MeshData plane = make_plane({100, 100});
    LightmapScene scene;
    scene.instances.push_back({&plane});
    LightmapSettings s = fast_settings();
    s.texels_per_unit = 16.0f; // 1600 texels wide
    s.max_atlas_size = 256;
    auto layout = unwrap_lightmaps(scene, s);
    REQUIRE(layout);
    CHECK(layout.value().width <= 256);
    CHECK(layout.value().height <= 256);
    CHECK(layout.value().texels_per_unit < 16.0f);
    s.texels_per_unit = 0.0f;
    CHECK_FALSE(unwrap_lightmaps(scene, s));
}

TEST_CASE("lightmap furnace: an open plane under a uniform sky stores the sky radiance") {
    // Irradiance from a uniform sky of radiance L is pi * L, and the lightmap stores E / pi = L.
    const MeshData plane = make_plane({2, 2});
    LightmapScene scene;
    scene.instances.push_back({&plane});
    scene.sky = uniform_sky(0.7f);
    auto bake = bake_lightmaps(scene, fast_settings());
    REQUIRE(bake);
    const auto v = lightmap_at(scene, bake.value(), 0, {0.3f, 0, -0.2f});
    REQUIRE(v);
    CHECK(v->x == doctest::Approx(0.7f).epsilon(0.02));
    CHECK(v->y == doctest::Approx(0.7f).epsilon(0.02));
    CHECK(bake.value().stats.texels > 0);
    CHECK(bake.value().stats.rays > 0);
}

TEST_CASE("lightmap include_direct bakes the sun as radiance * N.L / pi") {
    const MeshData plane = make_plane({2, 2});
    LightmapScene scene;
    scene.instances.push_back({&plane});
    scene.sky = uniform_sky(0.0f);
    LightmapLight sun;
    sun.direction = math::normalize(Vec3{0, -1, -1}); // 45 degrees
    sun.radiance = {2, 2, 2};
    scene.lights.push_back(sun);
    LightmapSettings s = fast_settings();
    s.include_direct = true;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    const auto v = lightmap_at(scene, bake.value(), 0, {0, 0, 0});
    REQUIRE(v);
    CHECK(v->x == doctest::Approx(2.0f * std::sqrt(0.5f) / math::kPi).epsilon(0.01));
    // Indirect mode leaves the receiver's direct light to the realtime renderer.
    s.include_direct = false;
    auto indirect = bake_lightmaps(scene, s);
    REQUIRE(indirect);
    const auto w = lightmap_at(scene, indirect.value(), 0, {0, 0, 0});
    REQUIRE(w);
    CHECK(w->x == doctest::Approx(0.0f));
}

TEST_CASE("directional lightmap: dominant direction, directionality and rebalancing (ADR-0130)") {
    // A sun at 45 degrees, fully baked: all light arrives from one direction.
    const MeshData plane = make_plane({2, 2});
    LightmapScene scene;
    scene.instances.push_back({&plane});
    scene.sky = uniform_sky(0.0f);
    LightmapLight sun;
    sun.direction = math::normalize(Vec3{0, -1, -1});
    sun.radiance = {2, 2, 2};
    scene.lights.push_back(sun);
    LightmapSettings s = fast_settings();
    s.include_direct = true;
    s.bounces = 0;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    REQUIRE(bake.value().direction.width == bake.value().atlas.width);
    const auto t = direction_at(scene, bake.value(), 0, {0.2f, 0, 0.1f});
    REQUIRE(t);
    const Vec3 to_sun = -sun.direction;
    const Vec3 d = decode_direction(*t);
    CHECK(math::length(d) == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(math::dot(math::normalize(d), to_sun) > 0.999f);
    CHECK(t->w == doctest::Approx(0.5f * std::sqrt(0.5f) + 0.5f).epsilon(0.01));
    // The baked normal gets the stored irradiance back; a normal-mapped pixel facing the sun gets
    // more (N.L = 1 instead of cos 45), one facing away gets less.
    CHECK(lightmap_directional_scale(*t, {0, 1, 0}) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(lightmap_directional_scale(*t, to_sun) > 1.15f);
    CHECK(lightmap_directional_scale(*t, math::normalize(Vec3{0, 1, -1})) < 0.65f);
    CHECK(lightmap_directional_scale({0.5f, 0.5f, 0.5f, 0.0f}, {1, 0, 0}) == 1.0f); // no data

    // A uniform sky: light from the whole hemisphere, mean cosine-weighted direction (0, 2/3, 0).
    scene.lights.clear();
    scene.sky = uniform_sky(1.0f);
    s.include_direct = false;
    s.samples = 256;
    auto sky = bake_lightmaps(scene, s);
    REQUIRE(sky);
    const auto u = direction_at(scene, sky.value(), 0, {0.2f, 0, 0.1f});
    REQUIRE(u);
    const Vec3 du = decode_direction(*u);
    CHECK(du.y == doctest::Approx(2.0f / 3.0f).epsilon(0.05));
    CHECK(std::fabs(du.x) < 0.05f);
    CHECK(std::fabs(du.z) < 0.05f);

    // Directional output is optional.
    s.directional = false;
    auto plain = bake_lightmaps(scene, s);
    REQUIRE(plain);
    CHECK(plain.value().direction.empty());
}

TEST_CASE("lightmap contact darkening and colour bleeding") {
    const MeshData floor = make_plane({8, 8}, 4);
    const MeshData wall = make_cube({0.1f, 1.0f, 2.0f});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance red{&wall, math::Mat4::translation({0, 1.0f, 0})};
    red.albedo = {0.9f, 0.05f, 0.05f};
    red.receives = false;
    scene.instances.push_back(red);
    scene.sky = uniform_sky(0.2f);
    LightmapLight sun;
    sun.direction = math::normalize(Vec3{-1, -1, 0}); // lights the wall's +X face and the floor
    sun.radiance = {3, 3, 3};
    scene.lights.push_back(sun);
    LightmapSettings s = fast_settings();
    s.samples = 192;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    const auto near_wall = lightmap_at(scene, bake.value(), 0, {0.35f, 0, 0});
    const auto far = lightmap_at(scene, bake.value(), 0, {3.5f, 0, 0});
    REQUIRE(near_wall);
    REQUIRE(far);
    // Sky is partly blocked next to the wall (darker in green/blue, which the red wall barely
    // bounces) and the sunlit red wall tints the floor next to it.
    CHECK(near_wall->y < far->y * 0.95f);
    CHECK(near_wall->x / near_wall->y > 1.5f * (far->x / far->y));
}

TEST_CASE("lightmap shadowmask stores the baked sun visibility in the atlas alpha (ADR-0131)") {
    // A wide box above the floor, sun straight down: the floor under it is in the sun's shadow.
    const MeshData floor = make_plane({8, 8}, 4);
    const MeshData roof = make_cube({2, 0.1f, 2});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance occluder{&roof, math::Mat4::translation({0, 1.5f, 0})};
    occluder.receives = false;
    scene.instances.push_back(occluder);
    scene.sky = uniform_sky(0.3f);
    LightmapLight sun;
    sun.direction = {0, -1, 0};
    sun.radiance = {3, 3, 3};
    scene.lights.push_back(sun);
    LightmapSettings s = fast_settings();
    s.samples = 16;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    CHECK(math::dot(bake.value().shadowmask_direction, Vec3{0, -1, 0}) == doctest::Approx(1.0f));
    auto alpha_at = [&](const LightmapBake& b, Vec3 p) {
        const auto uv = lightmap_uv_at(scene, b, 0, p);
        REQUIRE(uv);
        return sample_lightmap(b.atlas, *uv).w;
    };
    CHECK(alpha_at(bake.value(), {0, 0, 0}) < 0.05f);       // under the roof
    CHECK(alpha_at(bake.value(), {3.0f, 0, 3.0f}) > 0.95f); // open floor
    // The penumbra is soft: some texel near the roof's edge is partly lit.
    bool partial = false;
    for (float x = 1.6f; x < 2.4f; x += 0.02f) { // the roof ends at x = 2 (half extent)
        const float a = alpha_at(bake.value(), {x, 0, 0});
        partial = partial || (a > 0.1f && a < 0.9f);
    }
    CHECK(partial);

    // The file keeps the light the mask belongs to.
    LightmapFile file;
    file.atlas = bake.value().atlas;
    file.shadowmask_direction = bake.value().shadowmask_direction;
    auto back = decode_lightmap_file(encode_lightmap_file(file));
    REQUIRE(back);
    CHECK(math::dot(back.value().shadowmask_direction, Vec3{0, -1, 0}) == doctest::Approx(1.0f));

    // No shadow-casting directional light (or shadowmask off): alpha is coverage again.
    s.shadowmask = false;
    auto plain = bake_lightmaps(scene, s);
    REQUIRE(plain);
    CHECK(math::length(plain.value().shadowmask_direction) == 0.0f);
    CHECK(alpha_at(plain.value(), {0, 0, 0}) == doctest::Approx(1.0f));
    LightmapFile none;
    none.atlas = plain.value().atlas;
    auto none_back = decode_lightmap_file(encode_lightmap_file(none));
    REQUIRE(none_back);
    CHECK(math::length(none_back.value().shadowmask_direction) == 0.0f);
}

TEST_CASE("lightmap texels inside geometry are rejected instead of leaking") {
    // A closed box sitting on a receiving floor: the floor under the box sees only back faces.
    const MeshData floor = make_plane({4, 4}, 8);
    const MeshData box = make_cube({1, 0.5f, 1});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance occluder{&box, math::Mat4::translation({0, 0.4f, 0})};
    occluder.receives = false;
    scene.instances.push_back(occluder);
    scene.sky = uniform_sky(1.0f);
    LightmapSettings s = fast_settings();
    s.denoise = false;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    CHECK(bake.value().stats.invalid_texels > 0);
}

TEST_CASE("lightmap bake does not depend on the number of threads") {
    const MeshData floor = make_plane({4, 4}, 2);
    const MeshData cube = make_cube();
    LightmapScene scene;
    scene.instances.push_back({&floor});
    scene.instances.push_back({&cube, math::Mat4::translation({0, 0.5f, 0})});
    LightmapSettings s = fast_settings();
    s.samples = 16;
    auto single = bake_lightmaps(scene, s);
    jobs::JobSystem js(2);
    auto threaded = bake_lightmaps(scene, s, &js);
    REQUIRE(single);
    REQUIRE(threaded);
    CHECK(single.value().atlas.pixels == threaded.value().atlas.pixels);
    CHECK(single.value().direction.pixels == threaded.value().direction.pixels);
}

TEST_CASE("lightmap file round trip and damaged files") {
    const MeshData floor = make_plane({2, 2});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapSettings s = fast_settings();
    s.samples = 8;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    LightmapFile file;
    file.atlas = bake.value().atlas;
    file.direction = bake.value().direction;
    REQUIRE_FALSE(file.direction.empty());
    file.entries.push_back({Uuid{1, 2}, lightmap_mesh_fingerprint(floor), bake.value().instances[0]});
    const auto bytes = encode_lightmap_file(file);
    auto back = decode_lightmap_file(bytes);
    REQUIRE(back);
    CHECK(back.value().atlas.width == file.atlas.width);
    CHECK(back.value().atlas.height == file.atlas.height);
    REQUIRE(back.value().find(Uuid{1, 2}) != nullptr);
    CHECK(back.value().find(Uuid{3, 4}) == nullptr);
    CHECK(back.value().entries[0].unwrap.indices == file.entries[0].unwrap.indices);
    CHECK(back.value().entries[0].unwrap.uv == file.entries[0].unwrap.uv);
    CHECK(back.value().entries[0].mesh_fingerprint == file.entries[0].mesh_fingerprint);
    for (std::size_t i = 0; i < file.atlas.pixels.size(); ++i)
        CHECK(back.value().atlas.pixels[i] == doctest::Approx(file.atlas.pixels[i]).epsilon(0.002));
    CHECK(back.value().direction.pixels == file.direction.pixels);

    // Version 1 files (no flags word, no directional atlas) still load.
    LightmapFile v1_src = file;
    v1_src.direction = {};
    auto v1 = encode_lightmap_file(v1_src);
    v1[8] = 1;
    v1.erase(v1.begin() + 24, v1.begin() + 28); // magic 8 + version, width, height, count
    auto old = decode_lightmap_file(v1);
    REQUIRE(old);
    CHECK(old.value().direction.empty());
    CHECK(old.value().entries.size() == 1);
    // Unknown flags are rejected.
    auto flagged = bytes;
    flagged[24] |= 0x80;
    CHECK_FALSE(decode_lightmap_file(flagged));

    // Every truncation fails cleanly; a corrupted index is rejected.
    for (std::size_t n = 0; n < bytes.size(); n += std::max<std::size_t>(1, bytes.size() / 97)) {
        auto cut = decode_lightmap_file(std::span(bytes.data(), n));
        CHECK_FALSE(cut);
    }
    auto bad = bytes;
    bad[bad.size() - 1] = 0xff; // last index byte
    CHECK_FALSE(decode_lightmap_file(bad));
    auto version = bytes;
    version[8] = 9;
    auto v = decode_lightmap_file(version);
    REQUIRE_FALSE(v);
    CHECK(v.error().code == ErrorCode::VersionMismatch);
}

TEST_CASE("lightmap mesh fingerprint follows the geometry") {
    MeshData a = make_cube();
    const std::uint64_t fa = lightmap_mesh_fingerprint(a);
    CHECK(fa == lightmap_mesh_fingerprint(make_cube()));
    a.vertices[0].position.x += 0.01f;
    CHECK(fa != lightmap_mesh_fingerprint(a));
}
TEST_CASE("lightmap denoise removes noise but keeps contact shadows") {
    // A black cube on a floor under a uniform white sky: right at the cube the floor sees half
    // the sky (about 0.5), far away all of it (1).
    const MeshData floor = make_plane({6, 6}, 4);
    const MeshData cube = make_cube();
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance occ{&cube, math::Mat4::translation({0, 0.5f, 0})};
    occ.receives = false;
    occ.albedo = {0, 0, 0};
    scene.instances.push_back(occ);
    scene.sky = uniform_sky(1.0f);
    LightmapSettings s;
    s.texels_per_unit = 8;
    s.samples = 24;
    s.denoise = false;
    auto raw = bake_lightmaps(scene, s);
    s.denoise = true;
    auto clean = bake_lightmaps(scene, s);
    REQUIRE(raw);
    REQUIRE(clean);
    // Noise: spread of texels on an open strip of floor that should all be close to 1.
    auto spread = [&](const LightmapBake& b) {
        double sum = 0, sq = 0;
        int n = 0;
        for (float z = -2.5f; z <= 2.5f; z += 0.1f) {
            const auto v = lightmap_at(scene, b, 0, {2.6f, 0, z});
            REQUIRE(v);
            sum += v->x;
            sq += double(v->x) * v->x;
            ++n;
        }
        const double mean = sum / n;
        return std::sqrt(std::max(0.0, sq / n - mean * mean));
    };
    const double noisy = spread(raw.value()), smooth = spread(clean.value());
    MESSAGE("open floor spread raw " << noisy << ", denoised " << smooth);
    CHECK(smooth < noisy * 0.6);
    const auto contact = lightmap_at(scene, clean.value(), 0, {0.51f, 0, 0});
    const auto open = lightmap_at(scene, clean.value(), 0, {2.5f, 0, 0});
    REQUIRE(contact);
    REQUIRE(open);
    CHECK(contact->x < 0.62f);
    CHECK(open->x > 0.9f);
}

// --- Baked light probes (ADR-0133) -------------------------------------------------------------

TEST_CASE("light probes furnace: a uniform sky gives its radiance for every normal") {
    // A small far-away cube gives the tracer geometry; the probe box is open sky.
    const MeshData cube = make_cube({0.1f, 0.1f, 0.1f});
    LightmapScene scene;
    scene.instances.push_back({&cube, math::Mat4::translation({100, 0, 0})});
    scene.sky = uniform_sky(0.7f);
    LightmapSettings s = fast_settings();
    s.probe_bounds = {{-2, 0, -2}, {2, 2, 2}};
    s.probe_spacing = 2.0f;
    LightmapBakeStats stats;
    auto probes = bake_light_probes(scene, s, nullptr, &stats);
    REQUIRE(probes);
    CHECK(probes->counts == std::array<std::uint32_t, 3>{3, 3, 3}); // y at half spacing
    CHECK(probes->valid_count() == probes->probe_count());
    CHECK(stats.probes == 27);
    CHECK(stats.invalid_probes == 0);
    for (Vec3 n : {Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}, math::normalize(Vec3{1, 1, -1})}) {
        const Vec3 v = probes->sample({0.3f, 1.0f, -0.4f}, n, Vec3{-1});
        CHECK(v.x == doctest::Approx(0.7f).epsilon(0.01));
        CHECK(v.z == doctest::Approx(0.7f).epsilon(0.01));
    }
    // Outside the box the fallback is returned.
    CHECK(probes->sample({5, 1, 0}, {0, 1, 0}, Vec3{-1}).x == doctest::Approx(-1.0f));
}

TEST_CASE("light probes: fully baked mode adds the lights' direct light as SH, indirect mode leaves it out") {
    const MeshData cube = make_cube({0.1f, 0.1f, 0.1f});
    LightmapScene scene;
    scene.instances.push_back({&cube, math::Mat4::translation({100, 0, 0})});
    scene.sky = uniform_sky(0.0f);
    LightmapLight sun;
    sun.direction = {0, -1, 0};
    sun.radiance = {2, 2, 2};
    scene.lights.push_back(sun);
    LightmapSettings s = fast_settings();
    s.probe_bounds = {{-1, 0, -1}, {1, 2, 1}};
    s.include_direct = true;
    auto baked = bake_light_probes(scene, s);
    REQUIRE(baked);
    // SH9 of a delta light evaluated at its own direction: E * (1 + 2 + 5/4) / (4 pi), ~6 % above
    // the exact E / pi; facing away it rings slightly below zero and is clamped.
    const Vec3 up = baked->sample({0, 1, 0}, {0, 1, 0}, Vec3{0});
    CHECK(up.x == doctest::Approx(2.0f * 4.25f / (4.0f * math::kPi)).epsilon(0.01));
    CHECK(up.x > 2.0f / math::kPi);
    CHECK(baked->sample({0, 1, 0}, {0, -1, 0}, Vec3{0}).x < 0.05f);
    s.include_direct = false;
    auto indirect = bake_light_probes(scene, s);
    REQUIRE(indirect);
    CHECK(indirect->sample({0, 1, 0}, {0, 1, 0}, Vec3{0}).x == doctest::Approx(0.0f));
}

TEST_CASE("light probes inside geometry are invalid and skipped by sampling") {
    const MeshData box = make_cube({1, 1, 1}); // half extents
    LightmapScene scene;
    scene.instances.push_back({&box, math::Mat4::translation({0, 1, 0})}); // spans x,z -1..1, y 0..2
    scene.sky = uniform_sky(0.5f);
    LightmapSettings s = fast_settings();
    s.probe_bounds = {{-0.5f, 0.5f, -0.5f}, {3.5f, 1.5f, 3.5f}};
    s.probe_spacing = 2.0f; // x/z probes at -0.5, 1.5, 3.5
    s.probe_dilation = false;
    LightmapBakeStats stats;
    auto probes = bake_light_probes(scene, s, nullptr, &stats);
    REQUIRE(probes);
    // The (-0.5, y, -0.5) column is inside the box.
    CHECK(stats.invalid_probes == 2);
    CHECK(stats.dilated_probes == 0);
    CHECK(probes->valid[0] == 0);
    CHECK(probes->valid[1] == 1);
    // A point next to the invalid probes renormalizes over the valid ones and fades toward the
    // fallback (the sky) by the missing weight, exactly like pbr.frag: never darkened by black
    // inside-geometry probes.
    const Vec3 sky{0.5f};
    float lo = 1e9f;
    for (float x : {-0.5f, 1.5f})
        for (float y : {0.5f, 1.5f})
            for (float z : {-0.5f, 1.5f}) {
                if (x < 0 && z < 0) continue; // the invalid column
                lo = std::min(lo, probes->sample({x, y, z}, {0, 1, 0}, sky).x);
            }
    const Vec3 v = probes->sample({0.6f, 1.0f, 0.6f}, {0, 1, 0}, sky);
    MESSAGE("blend " << v.x << ", lowest valid corner " << lo);
    CHECK(lo > 0.0f);
    CHECK(v.x >= std::min(lo, sky.x) * 0.999f);
    // Entirely inside the invalid column there is nothing to interpolate: the fallback.
    CHECK(probes->sample({-0.5f, 1.0f, -0.5f}, {0, 1, 0}, sky).x == doctest::Approx(sky.x));
}

TEST_CASE("light probes inside geometry are dilated from their valid neighbours") {
    const MeshData box = make_cube({1, 1, 1});
    LightmapScene scene;
    scene.instances.push_back({&box, math::Mat4::translation({0, 1, 0})});
    scene.sky = uniform_sky(0.5f);
    LightmapSettings s = fast_settings();
    s.probe_bounds = {{-0.5f, 0.5f, -0.5f}, {3.5f, 1.5f, 3.5f}};
    s.probe_spacing = 2.0f;
    s.probe_dilation = false;
    auto raw = bake_light_probes(scene, s);
    REQUIRE(raw);
    s.probe_dilation = true;
    LightmapBakeStats stats;
    auto probes = bake_light_probes(scene, s, nullptr, &stats);
    REQUIRE(probes);
    CHECK(stats.invalid_probes == 2);
    CHECK(stats.dilated_probes == 2);
    CHECK(probes->valid_count() == probes->probe_count());
    // Valid probes are untouched; the dilated one is the weighted mean of its valid neighbours,
    // so it lies within their range rather than at the fallback.
    for (std::size_t i = 0; i < probes->sh.size(); ++i)
        if (raw->valid[i]) CHECK(probes->sh[i][0].x == raw->sh[i][0].x);
    float lo = 1e9f, hi = -1e9f;
    for (std::size_t i = 0; i < raw->sh.size(); ++i)
        if (raw->valid[i]) {
            lo = std::min(lo, raw->sh[i][0].x);
            hi = std::max(hi, raw->sh[i][0].x);
        }
    CHECK(probes->sh[0][0].x >= lo);
    CHECK(probes->sh[0][0].x <= hi);
    // The whole cell next to the box now interpolates at full weight: no fade to the fallback.
    const Vec3 v = probes->sample({0.6f, 1.0f, 0.6f}, {0, 1, 0}, Vec3{-1});
    CHECK(v.x > 0.3f);
    CHECK(v.x < 0.6f);
}

TEST_CASE("light probes: a roof shelters dynamic objects that the open sky would light") {
    const MeshData floor = make_plane({12, 12}, 4);
    const MeshData roof = make_cube({2, 0.1f, 2}); // 4 x 0.2 x 4 m slab, 2.1..2.3 m up
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance slab{&roof, math::Mat4::translation({0, 2.2f, 0})};
    slab.receives = false;
    scene.instances.push_back(slab);
    scene.sky = uniform_sky(1.0f);
    LightmapSettings s = fast_settings();
    s.probe_samples = 512;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    const LightmapProbes& p = bake->probes;
    REQUIRE_FALSE(p.empty());
    CHECK(bake->stats.probes == p.probe_count());
    CHECK(p.bounds.max.y - p.bounds.min.y >= 3.0f - 1e-3f); // tall enough for characters on a flat floor
    const Vec3 covered = p.sample({0, 1, 0}, {0, 1, 0}, Vec3{-1});
    const Vec3 open = p.sample({5, 1, 5}, {0, 1, 0}, Vec3{-1});
    MESSAGE("covered " << covered.x << ", open " << open.x);
    CHECK(covered.x >= 0.0f);
    CHECK(covered.x < open.x * 0.6f);
    CHECK(open.x == doctest::Approx(1.0f).epsilon(0.1));
}

TEST_CASE("light probes do not depend on the number of threads and can be turned off") {
    const MeshData floor = make_plane({4, 4}, 2);
    const MeshData cube = make_cube();
    LightmapScene scene;
    scene.instances.push_back({&floor});
    scene.instances.push_back({&cube, math::Mat4::translation({0, 0.5f, 0})});
    LightmapSettings s = fast_settings();
    s.samples = 8;
    s.probe_samples = 32;
    auto single = bake_lightmaps(scene, s);
    jobs::JobSystem js(2);
    auto threaded = bake_lightmaps(scene, s, &js);
    REQUIRE(single);
    REQUIRE(threaded);
    REQUIRE_FALSE(single->probes.empty());
    CHECK(single->probes.valid == threaded->probes.valid);
    for (std::size_t i = 0; i < single->probes.sh.size(); ++i)
        for (std::size_t k = 0; k < 9; ++k) CHECK(single->probes.sh[i][k] == threaded->probes.sh[i][k]);
    s.probes = false;
    auto none = bake_lightmaps(scene, s);
    REQUIRE(none);
    CHECK(none->probes.empty());
    CHECK(none->stats.probes == 0);
}

TEST_CASE("lightmap file round trip with light probes and damaged probe blocks") {
    const MeshData floor = make_plane({2, 2});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapSettings s = fast_settings();
    s.samples = 8;
    s.probe_samples = 16;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    REQUIRE_FALSE(bake->probes.empty());
    LightmapFile file;
    file.atlas = bake->atlas;
    file.probes = bake->probes;
    file.entries.push_back({Uuid{1, 2}, lightmap_mesh_fingerprint(floor), bake->instances[0]});
    const auto bytes = encode_lightmap_file(file);
    auto back = decode_lightmap_file(bytes);
    REQUIRE(back);
    const LightmapProbes& p = back->probes;
    CHECK(p.counts == file.probes.counts);
    CHECK(p.valid == file.probes.valid);
    CHECK(p.bounds.min.y == file.probes.bounds.min.y);
    for (std::size_t i = 0; i < p.sh.size(); ++i)
        for (std::size_t k = 0; k < 9; ++k) CHECK(p.sh[i][k].x == doctest::Approx(file.probes.sh[i][k].x).epsilon(0.002).scale(1e-3));
    CHECK(back->entries.size() == 1);

    // Header offsets: magic 8, version 8, w 12, h 16, count 20, flags 24, then the atlas.
    // A version 2 file must not carry the version 3 probe flag.
    auto v2 = bytes;
    v2[8] = 2;
    CHECK_FALSE(decode_lightmap_file(v2));
    // Probe counts out of range are rejected.
    const std::size_t probe_block = 28 + std::size_t{file.atlas.width} * file.atlas.height * 8;
    auto counts = bytes;
    counts[probe_block + 24] = 40; // counts[0] = 40 > 32
    CHECK_FALSE(decode_lightmap_file(counts));
    // A validity byte other than 0/1 is rejected.
    auto validity = bytes;
    validity[probe_block + 36] = 7;
    CHECK_FALSE(decode_lightmap_file(validity));
    // NaN bounds are rejected.
    auto nan = bytes;
    nan[probe_block + 0] = 0xff;
    nan[probe_block + 1] = 0xff;
    nan[probe_block + 2] = 0xff;
    nan[probe_block + 3] = 0x7f;
    CHECK_FALSE(decode_lightmap_file(nan));
    // Truncation anywhere in the probe block fails cleanly.
    for (std::size_t n = probe_block; n < probe_block + 36 + p.probe_count() * 55; n += 13)
        CHECK_FALSE(decode_lightmap_file(std::span(bytes.data(), n)));
}

TEST_CASE("light probes bake the shadowmask sun's visibility (ADR-0136)") {
    // A 4 x 4 m roof slab 2.1..2.3 m up, sun straight down: probes under it see no sun.
    const MeshData floor = make_plane({12, 12}, 4);
    const MeshData roof = make_cube({2, 0.1f, 2});
    LightmapScene scene;
    scene.instances.push_back({&floor});
    LightmapInstance slab{&roof, math::Mat4::translation({0, 2.2f, 0})};
    slab.receives = false;
    scene.instances.push_back(slab);
    scene.sky = uniform_sky(0.3f);
    LightmapLight sun;
    sun.direction = {0, -1, 0};
    sun.radiance = {3, 3, 3};
    scene.lights.push_back(sun);
    LightmapSettings s = fast_settings();
    s.samples = 8;
    s.probe_samples = 32;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    const LightmapProbes& p = bake->probes;
    REQUIRE(p.sun.size() == p.sh.size());
    for (std::size_t i = 0; i < p.sun.size(); ++i) {
        CHECK(p.sun[i] >= 0.0f);
        CHECK(p.sun[i] <= 1.0f);
    }
    const float covered = p.sample_sun({0, 1, 0}), open = p.sample_sun({5, 1, 5});
    MESSAGE("probe sun covered " << covered << ", open " << open);
    CHECK(covered < 0.05f);
    CHECK(open > 0.95f);
    CHECK(p.sample_sun({100, 1, 0}) == 1.0f); // outside the box: unshadowed
    // Thread count does not change it.
    jobs::JobSystem js(2);
    auto threaded = bake_light_probes(scene, s, &js);
    REQUIRE(threaded);
    REQUIRE(threaded->sun.size() == p.sun.size());
    for (std::size_t i = 0; i < p.sun.size(); ++i) CHECK(threaded->sun[i] == p.sun[i]);

    // File round trip (version 4), quantised to 1/255.
    LightmapFile file;
    file.atlas = bake->atlas;
    file.probes = p;
    file.shadowmask_direction = bake->shadowmask_direction; // no entries: the visibility bytes end the file
    const auto bytes = encode_lightmap_file(file);
    auto back = decode_lightmap_file(bytes);
    REQUIRE(back);
    REQUIRE(back->probes.sun.size() == p.sun.size());
    for (std::size_t i = 0; i < p.sun.size(); ++i) CHECK(back->probes.sun[i] == doctest::Approx(p.sun[i]).epsilon(0.003).scale(1.0));
    // A version 3 header must not carry the version 4 flag; the flag needs probes; truncated
    // visibility bytes fail cleanly.
    auto v3 = bytes;
    v3[8] = 3;
    CHECK_FALSE(decode_lightmap_file(v3));
    auto no_probes = bytes;
    no_probes[24] &= static_cast<std::uint8_t>(~4u);
    CHECK_FALSE(decode_lightmap_file(no_probes));
    for (std::size_t cut = 1; cut <= p.sun.size(); cut += 7) CHECK_FALSE(decode_lightmap_file(std::span(bytes.data(), bytes.size() - cut)));
    // Dropping the flag and the trailing bytes is a valid version 3 file without probe sun.
    auto old = std::vector<std::uint8_t>(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(p.sun.size()));
    old[8] = 3;
    old[24] &= static_cast<std::uint8_t>(~8u);
    auto old_back = decode_lightmap_file(old);
    REQUIRE(old_back);
    CHECK(old_back->probes.sun.empty());
    CHECK(old_back->probes.sample_sun({0, 1, 0}) == 1.0f);

    // No shadowmask light: no probe sun data.
    s.shadowmask = false;
    auto plain = bake_light_probes(scene, s);
    REQUIRE(plain);
    CHECK(plain->sun.empty());
}

TEST_CASE("lightmap unwrap shares uvs between congruent copies (ADR-0135)") {
    const MeshData plane = make_plane({10, 10}, 2);
    const MeshData cube = make_cube();
    const MeshData cube_copy = make_cube(); // a separate but identical mesh still shares
    const math::Quat quarter = math::Quat::axis_angle({0, 1, 0}, math::radians(90.0f));
    LightmapScene scene;
    scene.instances.push_back({&plane});
    scene.instances.push_back({&cube, math::Mat4::translation({-2, 0.5f, 0})});
    scene.instances.push_back({&cube_copy, math::Mat4::trs({2, 0.5f, 0}, quarter, {1, 1, 1})});
    scene.instances.push_back({&cube, math::Mat4::trs({0, 0.5f, 3}, {}, {2, 1, 1})}); // stretched: its own unwrap
    scene.sky = uniform_sky(0.7f);
    LightmapSettings s = fast_settings();
    s.texels_per_unit = 8.0f;
    auto bake = bake_lightmaps(scene, s);
    REQUIRE(bake);
    const auto& in = bake->instances;
    const auto ab = lightmap_uv_offset(in[1], in[2]);
    REQUIRE(ab);
    CHECK(std::abs(ab->x) + std::abs(ab->y) > 0.01f); // a different place in the atlas
    CHECK_FALSE(lightmap_uv_offset(in[1], in[3]));
    CHECK_FALSE(lightmap_uv_offset(in[0], in[1]));

    // No two receivers claim the same texel.
    const std::uint32_t W = bake->stats.width, H = bake->stats.height;
    std::vector<int> owner(std::size_t{W} * H, -1);
    int overlaps = 0;
    for (int ii = 0; ii < 4; ++ii) {
        const LightmapUnwrap& u = in[static_cast<std::size_t>(ii)];
        std::vector<std::uint8_t> mine(owner.size(), 0);
        for (std::size_t t = 0; t + 2 < u.indices.size(); t += 3) {
            Vec2 p[3];
            for (int j = 0; j < 3; ++j) p[j] = {u.uv[u.indices[t + j]].x * W, u.uv[u.indices[t + j]].y * H};
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            if (std::abs(area) < 1e-6f) continue;
            for (std::uint32_t y = 0; y < H; ++y)
                for (std::uint32_t x = 0; x < W; ++x) {
                    const Vec2 c{x + 0.5f, y + 0.5f};
                    const auto edge = [&](Vec2 a, Vec2 b) { return ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / area; };
                    if (edge(p[0], p[1]) >= 0 && edge(p[1], p[2]) >= 0 && edge(p[2], p[0]) >= 0) mine[std::size_t{y} * W + x] = 1;
                }
        }
        for (std::size_t k = 0; k < owner.size(); ++k) {
            if (!mine[k]) continue;
            if (owner[k] >= 0 && owner[k] != ii) ++overlaps;
            owner[k] = ii;
        }
    }
    CHECK(overlaps == 0);

    // The copies are lit alike at matching points: the shared layout is valid for both.
    for (const Vec3 local : {Vec3{0, 0.5f, 0}, Vec3{0.5f, 0, 0}, Vec3{0, 0, 0.5f}, Vec3{-0.5f, 0, 0}}) {
        const auto a = lightmap_at(scene, *bake, 1, scene.instances[1].model.transform_point(local));
        const auto b = lightmap_at(scene, *bake, 2, scene.instances[2].model.transform_point(local));
        REQUIRE(a);
        REQUIRE(b);
        MESSAGE("copy A " << a->x << ", copy B " << b->x);
        CHECK(std::abs(a->x - b->x) < 0.06f);
        CHECK(a->x > 0.2f);
    }

    // Sharing off: every receiver is unwrapped as before (no constant offset expected, and the
    // layout matches the single xatlas pass).
    s.share_uvs = false;
    auto separate = unwrap_lightmaps(scene, s);
    REQUIRE(separate);
    CHECK(separate->instances[1].indices.size() == cube.indices.size());
}
