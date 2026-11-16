// SPDX-License-Identifier: Apache-2.0
// Skins and animation clips in cooked models and glTF (Slice 12): cooked v2 round trip and
// validation, glTF export -> import round trip, importer details and the database provider.
#include "doctest.h"
#include "skinned_model.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"

#include <cmath>
#include <filesystem>

using namespace victus;
using namespace victus::assets;

namespace {

bool same(const math::Mat4& a, const math::Mat4& b, float eps = 1e-5f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a.c[c][r] - b.c[c][r]) > eps) return false;
    return true;
}

bool near_all(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > 1e-6f) return false;
    return true;
}

void check_column(const CookedModel& m) {
    REQUIRE(m.meshes.size() == 1);
    const auto& skin = m.meshes[0].data.skin;
    REQUIRE(skin.size() == m.meshes[0].data.vertices.size());
    // Bottom segment vertices on joint 0, top on joint 1, all weight 1.
    for (std::size_t i = 0; i < skin.size(); ++i) {
        const float y = m.meshes[0].data.vertices[i].position.y;
        CHECK(skin[i].joints[0] == (y > 1.0f + 1e-4f ? 1 : (y < 1.0f - 1e-4f ? 0 : skin[i].joints[0])));
        CHECK(skin[i].weights[0] == doctest::Approx(1.0f));
    }
    REQUIRE(m.skins.size() == 1);
    CHECK(m.skins[0].name == "Skeleton");
    REQUIRE(m.skins[0].joints.size() == 2);
    CHECK(m.nodes[m.skins[0].joints[0]].name == "Hip");
    CHECK(m.nodes[m.skins[0].joints[1]].name == "Spine");
    CHECK(same(m.skins[0].inverse_bind[1], math::Mat4::translation({0, -1, 0})));
    std::int32_t skinned_nodes = 0;
    for (const auto& n : m.nodes) skinned_nodes += n.skin == 0 ? 1 : 0;
    CHECK(skinned_nodes == 1);
    REQUIRE(m.animations.size() == 2);
    const animation::Clip& bend = m.animations[0];
    CHECK(bend.name == "Bend");
    CHECK(bend.duration == doctest::Approx(1.0f));
    REQUIRE(bend.channels.size() == 1);
    CHECK(m.nodes[bend.channels[0].node].name == "Spine");
    CHECK(bend.channels[0].path == animation::Path::Rotation);
    CHECK(bend.channels[0].values.size() == 8);
    const animation::Clip& lift = m.animations[1];
    CHECK(lift.channels[0].interpolation == animation::Interpolation::Step);
    CHECK(lift.duration == doctest::Approx(2.0f));
    CHECK(validate_model(m).ok());
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

TEST_CASE("assets: cooked model v2 keeps skins and animations and rejects bad references") {
    const CookedModel model = test::make_skinned_column();
    check_column(model);
    CookedHeader h;
    const auto bytes = serialize(h, model);
    auto back = deserialize_model(bytes);
    REQUIRE(back.ok());
    check_column(*back);
    CHECK(back->meshes[0].data.skin[5].joints[0] == model.meshes[0].data.skin[5].joints[0]);
    for (std::size_t n = 0; n < bytes.size(); n += 61) CHECK_FALSE(deserialize_model(std::span(bytes.data(), n)).ok());

    auto rejects = [&](auto&& mutate) {
        CookedModel bad = test::make_skinned_column();
        mutate(bad);
        CHECK_FALSE(validate_model(bad).ok());
        CHECK_FALSE(deserialize_model(serialize(h, bad)).ok());
    };
    rejects([](CookedModel& m) { m.skins[0].joints[1] = 99; });                 // joint node out of range
    rejects([](CookedModel& m) { m.skins[0].inverse_bind.pop_back(); });        // matrix count
    rejects([](CookedModel& m) { m.nodes[0].skin = 4; });                       // node skin index
    rejects([](CookedModel& m) { m.meshes[0].data.skin.pop_back(); });          // stream size
    rejects([](CookedModel& m) { m.meshes[0].data.skin[0].joints[0] = 2; });    // vertex joint >= skin joints
    rejects([](CookedModel& m) { m.animations[0].channels[0].node = 3; });      // clip node
    rejects([](CookedModel& m) { m.animations[1].channels[0].times = {0, 2, 1}; }); // unsorted keys

    // Version bump: v1 files are reported as a version mismatch (the database re-imports them).
    auto old = bytes;
    old[5] = 1;
    auto r = deserialize_model(old);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::VersionMismatch);
}

TEST_CASE("assets: glTF export -> import round trip with skins and animations") {
    TempDir dir("victus_skin_rt");
    const CookedModel model = test::make_skinned_column();
    const fs::Path file = dir.path / "column.gltf";
    REQUIRE(export_gltf(model, file).ok());
    ImportResult res;
    auto imported = import_gltf(file, {}, dir.path, &res);
    REQUIRE(imported.ok());
    for (const auto& w : res.warnings) MESSAGE("warning: " << w);
    CHECK(res.warnings.empty());
    check_column(*imported);
    CHECK(imported->meshes[0].data.vertices.size() == model.meshes[0].data.vertices.size());
    CHECK(near_all(imported->animations[0].channels[0].values, model.animations[0].channels[0].values));

    // The import scale applies to root nodes and to their translation/scale animation.
    ModelImportSettings scaled;
    scaled.scale = 2.0f;
    auto big = import_gltf(file, scaled, dir.path);
    REQUIRE(big.ok());
    const auto& lift = big->animations[1].channels[0];
    CHECK(lift.values[7] == doctest::Approx(2.0f)); // Hip (a root) rises 2 m at scale 2
    CHECK(near_all(big->animations[0].channels[0].values, model.animations[0].channels[0].values)); // rotations unscaled

    const animation::AnimationSet set = make_animation_set(*imported);
    CHECK(set.nodes.size() == 3);
    CHECK(set.nodes[2].parent == 1);
    CHECK(set.find_clip("Lift") == 1);

    // Exporting an invalid model fails instead of writing a broken file.
    CookedModel bad = model;
    bad.skins[0].joints[0] = 50;
    CHECK_FALSE(export_gltf_json(bad).ok());
}

TEST_CASE("assets: importer rejects dangling joints and renders over-indexed skins unskinned") {
    TempDir dir("victus_skin_bad");
    auto text = export_gltf_json(test::make_skinned_column());
    REQUIRE(text.ok());
    auto with_joints = [&](std::initializer_list<int> joints) {
        auto doc = json::parse(*text);
        REQUIRE(doc.ok());
        json::Value list = json::Value::array();
        for (const int j : joints) list.push(j);
        for (auto& [key, value] : doc->as_object())
            if (key == "skins") value.as_array()[0].set("joints", list);
        return json::stringify(*doc);
    };
    // A joint referencing a node that does not exist fails validation.
    REQUIRE(fs::write_text_atomic(dir.path / "dangling.gltf", with_joints({7})).ok());
    CHECK_FALSE(import_gltf(dir.path / "dangling.gltf", {}, dir.path).ok());

    // One joint but vertices use joint 1: the node is drawn unskinned (with a warning) instead
    // of reading past the joint palette.
    REQUIRE(fs::write_text_atomic(dir.path / "short.gltf", with_joints({1})).ok());
    ImportResult res;
    auto r = import_gltf(dir.path / "short.gltf", {}, dir.path, &res);
    REQUIRE(r.ok());
    CHECK(r->nodes[0].skin == -1);
    bool warned = false;
    for (const auto& w : res.warnings) warned |= w.find("rendered unskinned") != std::string::npos;
    CHECK(warned);
    CHECK(validate_model(*r).ok());
}

TEST_CASE("assets: database animation provider serves imported clips") {
    TempDir dir("victus_skin_db");
    std::filesystem::create_directories(dir.path / "assets" / "models");
    REQUIRE(export_gltf(test::make_skinned_column(), dir.path / "assets" / "models" / "column.gltf").ok());
    auto db = AssetDatabase::open(dir.path);
    REQUIRE(db.ok());
    const AssetRecord* rec = (*db)->find_path("models/column.gltf");
    REQUIRE(rec);
    const auto provider = animation_provider(**db);
    const auto set = provider(rec->uuid);
    REQUIRE(set);
    CHECK(set->clips.size() == 2);
    CHECK(set->nodes[1].name == "Hip");
    CHECK(provider(Uuid::generate()) == nullptr);
}
