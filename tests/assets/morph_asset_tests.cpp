// SPDX-License-Identifier: Apache-2.0
// Morph targets in cooked models and glTF (ADR-0076): cooked v7 round trip and validation,
// glTF export -> import round trip (targets, names, default and node weights, weight channels,
// optimisation remap) and the animation set built from them.
#include "doctest.h"
#include "victus/assets/cooked.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/fs.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace victus;
using namespace victus::assets;

namespace {

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// A 2x2 plane "Face" with targets "Raise" (vertices with x > 0 rise 1 m, normals tilt) and "Push"
// (every vertex moves +0.5 in z, positions only); default weights 0.5 / 0, and a "Talk" clip
// animating both weights 0 -> 1 over 1 s.
CookedModel make_face() {
    CookedModel m;
    ModelMaterial mat;
    mat.name = "Skin";
    m.materials.push_back(mat);
    render::MeshData mesh = render::make_plane({2, 2}, 2);
    render::MorphTarget raise{.name = "Raise", .vertices = {}, .positions = {}, .normals = {}};
    render::MorphTarget push{.name = "Push", .vertices = {}, .positions = {}, .normals = {}};
    for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i) {
        if (mesh.vertices[i].position.x > 0.0f) {
            raise.vertices.push_back(i);
            raise.positions.push_back({0, 1, 0});
            raise.normals.push_back({0.5f, 0, 0});
        }
        push.vertices.push_back(i);
        push.positions.push_back({0, 0, 0.5f});
    }
    mesh.morphs = {raise, push};
    mesh.morph_weights = {0.5f, 0.0f};
    m.meshes.push_back({"Face#0", std::move(mesh), 0});
    ModelNode node;
    node.name = "Face";
    node.meshes = {0};
    m.nodes.push_back(node);
    animation::Channel ch{.node = 0, .path = animation::Path::Weights, .interpolation = animation::Interpolation::Linear,
                          .times = {0, 1}, .values = {0, 0, 1, 1}};
    ch.width = 2;
    m.animations.push_back({.name = "Talk", .duration = 1, .channels = {ch}});
    return m;
}

// Morphed positions as a sorted list (vertex order changes when the importer optimises).
std::vector<std::array<float, 3>> morphed_positions(const render::MeshData& mesh, std::vector<float> weights) {
    std::vector<render::Vertex3D> out;
    render::apply_morphs(mesh, weights, out);
    std::vector<std::array<float, 3>> p;
    for (const auto& v : out)
        p.push_back({std::round(v.position.x * 1e4f) / 1e4f, std::round(v.position.y * 1e4f) / 1e4f, std::round(v.position.z * 1e4f) / 1e4f});
    std::sort(p.begin(), p.end());
    return p;
}

} // namespace

TEST_CASE("assets: cooked models keep morph targets and weight channels and reject corrupt ones") {
    const CookedModel model = make_face();
    REQUIRE(validate_model(model).ok());
    CookedHeader h;
    const auto bytes = serialize(h, model);
    auto back = deserialize_model(bytes);
    REQUIRE(back.ok());
    const render::MeshData& d = back->meshes[0].data;
    REQUIRE(d.morphs.size() == 2);
    CHECK(d.morphs[0].name == "Raise");
    CHECK(d.morphs[0].vertices == model.meshes[0].data.morphs[0].vertices);
    CHECK(d.morphs[0].normals.size() == d.morphs[0].vertices.size());
    CHECK(d.morphs[1].normals.empty());
    CHECK(d.morph_weights == model.meshes[0].data.morph_weights);
    REQUIRE(back->animations.size() == 1);
    CHECK(back->animations[0].channels[0].path == animation::Path::Weights);
    CHECK(back->animations[0].channels[0].width == 2);
    for (std::size_t n = 0; n < bytes.size(); n += 37) CHECK_FALSE(deserialize_model(std::span(bytes.data(), n)).ok());

    auto rejects = [&](auto&& mutate) {
        CookedModel bad = make_face();
        mutate(bad);
        CHECK_FALSE(validate_model(bad).ok());
        CHECK_FALSE(deserialize_model(serialize(h, bad)).ok());
    };
    rejects([](CookedModel& m) { m.meshes[0].data.morphs[0].vertices.back() = 9999; });          // vertex out of range
    rejects([](CookedModel& m) { std::swap(m.meshes[0].data.morphs[1].vertices[0], m.meshes[0].data.morphs[1].vertices[1]); }); // order
    rejects([](CookedModel& m) { m.meshes[0].data.morphs[1].positions.pop_back(); });             // delta count
    rejects([](CookedModel& m) { m.meshes[0].data.morph_weights.push_back(1); });                 // weight count
    rejects([](CookedModel& m) { m.meshes[0].data.morphs[0].positions[0].x = std::nanf(""); });   // non-finite
    rejects([](CookedModel& m) { m.animations[0].channels[0].width = 0; });                       // channel width
}

TEST_CASE("assets: glTF export -> import round trip with morph targets and weight animation") {
    TempDir dir("victus_morph_rt");
    CookedModel model = make_face();
    model.nodes[0].morph_weights = {0.25f, 1.0f}; // node.weights override the mesh defaults
    const fs::Path file = dir.path / "face.gltf";
    REQUIRE(export_gltf(model, file).ok());
    ImportResult res;
    auto imported = import_gltf(file, {}, dir.path, &res);
    REQUIRE(imported.ok());
    for (const auto& w : res.warnings) MESSAGE("warning: " << w);
    CHECK(res.warnings.empty());
    REQUIRE(imported->meshes.size() == 1);
    const render::MeshData& d = imported->meshes[0].data;
    REQUIRE(d.morphs.size() == 2);
    CHECK(d.morphs[0].name == "Raise");
    CHECK(d.morphs[1].name == "Push");
    CHECK(d.morph_weights == std::vector<float>{0.5f, 0.0f});
    CHECK(d.morphs[0].vertices.size() == model.meshes[0].data.morphs[0].vertices.size()); // zero deltas stay sparse
    CHECK(render::validate_morphs(d).empty());
    // Same shapes after export, import and vertex reordering.
    for (const std::vector<float>& w : {std::vector<float>{1, 0}, std::vector<float>{0, 1}, std::vector<float>{0.3f, -0.7f}})
        CHECK(morphed_positions(d, w) == morphed_positions(model.meshes[0].data, w));
    CHECK(imported->nodes[0].morph_weights == std::vector<float>{0.25f, 1.0f});
    REQUIRE(imported->animations.size() == 1);
    const animation::Channel& ch = imported->animations[0].channels.at(0);
    CHECK(ch.path == animation::Path::Weights);
    CHECK(ch.width == 2);
    CHECK(ch.values == std::vector<float>{0, 0, 1, 1});

    // The animation set carries one weight node per target, rest = node weights.
    const animation::AnimationSet set = make_animation_set(*imported);
    REQUIRE(set.nodes.size() == 3);
    CHECK(set.nodes[1].name == "Face/Raise");
    CHECK(set.nodes[1].rest.translation.x == doctest::Approx(0.25f));
    CHECK(set.nodes[2].rest.translation.x == doctest::Approx(1.0f));
    REQUIRE(set.mesh_morphs.size() == 1);
    CHECK(set.mesh_morphs[0].names == std::vector<std::string>{"Raise", "Push"});
    CHECK(set.mesh_morphs[0].defaults == std::vector<float>{0.5f, 0.0f});
    REQUIRE(set.clips[0].channels.size() == 2);
    CHECK(set.clips[0].channels[0].path == animation::Path::Translation);
    CHECK(set.clips[0].channels[1].node == 2);
}

TEST_CASE("render: morph targets apply, survive mesh optimisation and validate") {
    render::MeshData mesh = make_face().meshes[0].data;
    std::vector<render::Vertex3D> out;
    CHECK_FALSE(render::apply_morphs(mesh, std::vector<float>{0, 0}, out)); // all zero: base copy
    CHECK(out.size() == mesh.vertices.size());
    REQUIRE(render::apply_morphs(mesh, std::vector<float>{1, 0.5f}, out));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const bool raised = mesh.vertices[i].position.x > 0.0f;
        CHECK(out[i].position.y == doctest::Approx(mesh.vertices[i].position.y + (raised ? 1.0f : 0.0f)));
        CHECK(out[i].position.z == doctest::Approx(mesh.vertices[i].position.z + 0.25f));
        CHECK(math::length(out[i].normal) == doctest::Approx(1.0f)); // renormalised
        CHECK(std::abs(math::dot(out[i].normal, out[i].tangent.xyz())) < 1e-4f); // tangent re-orthogonalised
    }
    // Fewer weights than targets: the rest count as 0.
    REQUIRE(render::apply_morphs(mesh, std::vector<float>{0, 1}, out));
    CHECK(out[0].position.z == doctest::Approx(mesh.vertices[0].position.z + 0.5f));

    const auto before = morphed_positions(mesh, {0.7f, 0.2f});
    render::LodSettings lod;
    render::generate_lods(mesh, lod);
    render::optimize_mesh(mesh);
    CHECK(render::validate_morphs(mesh).empty());
    CHECK(morphed_positions(mesh, {0.7f, 0.2f}) == before);

    render::MeshData merged = mesh;
    merged.append(render::make_cube(), math::Mat4::identity());
    CHECK(merged.morphs.empty()); // appended geometry has no deltas: targets are dropped
}

TEST_CASE("assets: skeleton-only and geometry-only decoders match the full decoder") {
    const CookedModel model = make_face();
    CookedHeader h;
    const auto bytes = serialize(h, model);
    auto full = deserialize_model(bytes);
    auto skel = deserialize_model_skeleton(bytes);
    REQUIRE(full.ok());
    REQUIRE(skel.ok());
    CHECK(skel->textures.empty());
    REQUIRE(skel->meshes.size() == 1);
    CHECK(skel->meshes[0].data.vertices.empty());
    CHECK(skel->meshes[0].data.indices.empty());
    REQUIRE(skel->meshes[0].data.morphs.size() == 2);
    CHECK(skel->meshes[0].data.morphs[1].name == "Push");
    CHECK(skel->meshes[0].data.morphs[1].positions.empty());

    const animation::AnimationSet a = make_animation_set(*full);
    const animation::AnimationSet b = make_animation_set(*skel);
    REQUIRE(a.nodes.size() == b.nodes.size());
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        CHECK(a.nodes[i].name == b.nodes[i].name);
        CHECK(a.nodes[i].parent == b.nodes[i].parent);
        CHECK(a.nodes[i].morph_node == b.nodes[i].morph_node);
        CHECK(a.nodes[i].morph_index == b.nodes[i].morph_index);
        CHECK(a.nodes[i].rest.translation.x == b.nodes[i].rest.translation.x);
    }
    REQUIRE(a.clips.size() == b.clips.size());
    CHECK(a.clips[0].name == b.clips[0].name);
    CHECK(a.clips[0].channels.size() == b.clips[0].channels.size());
    REQUIRE(a.mesh_morphs.size() == b.mesh_morphs.size());
    CHECK(a.mesh_morphs[0].names == b.mesh_morphs[0].names);
    CHECK(a.mesh_morphs[0].defaults == b.mesh_morphs[0].defaults);

    auto geo = deserialize_model_geometry(bytes);
    REQUIRE(geo.ok());
    CHECK(geo->textures.empty());
    CHECK(geo->meshes[0].data.vertices.size() == full->meshes[0].data.vertices.size());
    CHECK(geo->meshes[0].data.indices == full->meshes[0].data.indices);
    CHECK(geo->materials[0].base_color_texture == -1);

    for (std::size_t n = 0; n < bytes.size(); n += 37) CHECK_FALSE(deserialize_model_skeleton(std::span(bytes.data(), n)).ok());
    // The exported sidecar decodes to the same skeleton and is a fraction of the model.
    auto sidecar = make_skeleton_sidecar(bytes);
    REQUIRE(sidecar.ok());
    CHECK(sidecar->size() < bytes.size() / 2);
    auto side = deserialize_model_skeleton(*sidecar);
    REQUIRE(side.ok());
    const animation::AnimationSet c = make_animation_set(*side);
    REQUIRE(c.nodes.size() == a.nodes.size());
    for (std::size_t i = 0; i < a.nodes.size(); ++i) CHECK(c.nodes[i].name == a.nodes[i].name);
    REQUIRE(c.clips.size() == a.clips.size());
    CHECK(c.clips[0].channels.size() == a.clips[0].channels.size());
    CHECK(c.mesh_morphs[0].names == a.mesh_morphs[0].names);
    CHECK(c.mesh_morphs[0].defaults == a.mesh_morphs[0].defaults);
    CHECK_FALSE(make_skeleton_sidecar(std::span(bytes.data(), bytes.size() / 2)).ok());

    CookedModel bad = make_face();
    bad.animations[0].channels[0].node = 7; // channel targets a missing node
    CHECK_FALSE(deserialize_model_skeleton(serialize(h, bad)).ok());
}

TEST_CASE("fs: map_file maps loose files and falls back for empty ones") {
    TempDir dir("victus_map_file");
    const fs::Path file = dir.path / "data.bin";
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5, 250};
    REQUIRE(fs::write_atomic(file, payload).ok());
    auto view = fs::map_file(file);
    REQUIRE(view.ok());
    CHECK(std::equal(view->bytes().begin(), view->bytes().end(), payload.begin(), payload.end()));
    fs::FileView moved = std::move(*view);
    CHECK(moved.bytes().size() == payload.size());
    CHECK(moved.bytes()[5] == 250);
    // Replacing the file (write_atomic renames) leaves the existing view on the old bytes.
    REQUIRE(fs::write_atomic(file, std::vector<std::uint8_t>{9}).ok());
    CHECK(moved.bytes().size() == payload.size());
    CHECK(fs::map_file(file)->bytes().size() == 1);

    const fs::Path empty = dir.path / "empty.bin";
    REQUIRE(fs::write_atomic(empty, std::vector<std::uint8_t>{}).ok());
    auto e = fs::map_file(empty);
    REQUIRE(e.ok());
    CHECK(e->bytes().empty());
    CHECK_FALSE(e->mapped());
    CHECK_FALSE(fs::map_file(dir.path / "missing.bin").ok());
    CHECK_FALSE(fs::map_file(file, 0).ok()); // over the size limit
}
