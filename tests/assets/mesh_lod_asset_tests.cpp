// SPDX-License-Identifier: Apache-2.0
// Mesh LODs in the asset pipeline (Slice 54, ADR-0049): cooked v6 keeps LODs and rejects corrupt
// ones; the glTF importer optimises meshes and builds LODs per the model import settings.
#include "doctest.h"
#include "victus/assets/cooked.hpp"
#include "victus/assets/database.hpp"
#include "victus/assets/importers.hpp"
#include "victus/core/json.hpp"

#include <filesystem>

using namespace victus;
using namespace victus::assets;

namespace {

CookedModel dense_ball() {
    CookedModel m;
    ModelMaterial mat;
    mat.name = "Ball";
    m.materials.push_back(mat);
    m.meshes.push_back({"Ball#0", render::make_uv_sphere(1.0f, 48, 24), 0});
    ModelNode node;
    node.name = "Ball";
    node.meshes = {0};
    m.nodes.push_back(node);
    return m;
}

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

} // namespace

TEST_CASE("assets: cooked model v6 keeps mesh LODs and rejects corrupt ones") {
    CookedModel model = dense_ball();
    REQUIRE(render::generate_lods(model.meshes[0].data) == 3);
    CookedHeader h;
    const auto bytes = serialize(h, model);
    auto back = deserialize_model(bytes);
    REQUIRE(back.ok());
    const auto& lods = back->meshes[0].data.lods;
    REQUIRE(lods.size() == 3);
    for (std::size_t i = 0; i < lods.size(); ++i) {
        CHECK(lods[i].indices == model.meshes[0].data.lods[i].indices);
        CHECK(lods[i].error == model.meshes[0].data.lods[i].error);
    }
    for (std::size_t n = 0; n < bytes.size(); n += 997) CHECK_FALSE(deserialize_model(std::span(bytes.data(), n)).ok());

    auto rejects = [&](auto&& mutate) {
        CookedModel bad = model;
        mutate(bad);
        CHECK_FALSE(deserialize_model(serialize(h, bad)).ok());
    };
    rejects([](CookedModel& m) { m.meshes[0].data.lods[1].indices.pop_back(); });     // not triangles
    rejects([](CookedModel& m) { m.meshes[0].data.lods[0].indices[0] = 1u << 30; });  // out of range
    rejects([](CookedModel& m) { m.meshes[0].data.lods[2].error = -1.0f; });          // negative error
    rejects([](CookedModel& m) { m.meshes[0].data.lods.resize(render::kMaxMeshLods); }); // too many

    // Older cooked versions are reported as a version mismatch (the database re-imports them).
    auto old = bytes;
    old[5] = 5;
    auto r = deserialize_model(old);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::VersionMismatch);
}

TEST_CASE("assets: the glTF importer optimises meshes and builds LODs") {
    TempDir dir("victus_lod_import");
    const fs::Path file = dir.path / "ball.gltf";
    REQUIRE(export_gltf(dense_ball(), file).ok());
    const std::size_t full = dense_ball().meshes[0].data.triangle_count();

    auto imported = import_gltf(file, {}, dir.path);
    REQUIRE(imported.ok());
    const render::MeshData& mesh = imported->meshes[0].data;
    CHECK(mesh.triangle_count() == full);
    REQUIRE(mesh.lods.size() == 3);
    CHECK(mesh.lods.back().indices.size() / 3 < full / 4);

    ModelImportSettings one;
    one.lods = 1;
    auto single = import_gltf(file, one, dir.path);
    REQUIRE(single.ok());
    CHECK(single->meshes[0].data.lods.size() == 1);

    ModelImportSettings raw;
    raw.optimize_meshes = false;
    auto plain = import_gltf(file, raw, dir.path);
    REQUIRE(plain.ok());
    CHECK(plain->meshes[0].data.lods.empty());
    CHECK(plain->meshes[0].data.triangle_count() == full);

    // Settings round trip; defaults are not written, so existing .meta files stay unchanged.
    CHECK(ModelImportSettings{}.to_json().find("optimize_meshes") == nullptr);
    CHECK(ModelImportSettings{}.to_json().find("lods") == nullptr);
    ModelImportSettings custom;
    custom.optimize_meshes = false;
    custom.lods = 5;
    custom.lod_error = 0.1f;
    const auto parsed = ModelImportSettings::from_json(custom.to_json());
    CHECK_FALSE(parsed.optimize_meshes);
    CHECK(parsed.lods == 5);
    CHECK(parsed.lod_error == doctest::Approx(0.1f));
    auto clamped = ModelImportSettings::from_json(json::parse(R"({"lods": 99, "lod_error": -3})").value());
    CHECK(clamped.lods == render::kMaxMeshLods - 1);
    CHECK(clamped.lod_error == doctest::Approx(0.05f));
}
