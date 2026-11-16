// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/scene/scene.hpp"

#include <filesystem>

using namespace victus;
using namespace victus::scene;
using math::Quat;
using math::Vec3;

namespace {
bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return math::length(a - b) < eps; }
std::vector<std::string> names(const Scene& s, const std::vector<Entity>& es) {
    std::vector<std::string> out;
    for (const Entity e : es) out.push_back(s.world().get<Name>(e)->value);
    return out;
}
} // namespace

TEST_CASE("scene: hierarchy ordering, reparenting and cycle rejection") {
    Scene s;
    const Entity a = s.create("A");
    const Entity b = s.create("B");
    const Entity a1 = s.create("A1", a);
    const Entity a2 = s.create("A2", a);
    const Entity a3 = s.create("A3", a);
    CHECK(names(s, s.roots()) == std::vector<std::string>{"A", "B"});
    CHECK(names(s, s.children(a)) == std::vector<std::string>{"A1", "A2", "A3"});
    CHECK(s.world().get<Hierarchy>(a2)->depth == 1);

    CHECK(s.set_parent(a2, b));
    CHECK(names(s, s.children(a)) == std::vector<std::string>{"A1", "A3"});
    CHECK(names(s, s.children(b)) == std::vector<std::string>{"A2"});
    CHECK_FALSE(s.set_parent(a, a1)); // cycle
    CHECK_FALSE(s.set_parent(a, a));
    CHECK(s.set_parent(b, a1));
    CHECK(s.world().get<Hierarchy>(a2)->depth == 3);
    CHECK(s.is_descendant(a2, a));
    CHECK(names(s, s.roots()) == std::vector<std::string>{"A"});

    s.destroy(a1); // removes a1, b, a2
    CHECK(s.size() == 2);
    CHECK_FALSE(s.world().alive(b));
    CHECK_FALSE(s.world().alive(a2));
    CHECK(names(s, s.children(a)) == std::vector<std::string>{"A3"});
    CHECK(s.find_by_name("A3") == a3);
}

TEST_CASE("scene: world transforms propagate parent before child") {
    Scene s;
    const Entity root = s.create("root");
    const Entity child = s.create("child", root);
    const Entity grandchild = s.create("grandchild", child);
    s.world().get<Transform>(root)->position = {10, 0, 0};
    s.world().get<Transform>(root)->rotation = Quat::axis_angle({0, 1, 0}, math::radians(90));
    s.world().get<Transform>(child)->position = {0, 0, -2};
    s.world().get<Transform>(child)->scale = {2, 2, 2};
    s.world().get<Transform>(grandchild)->position = {1, 0, 0};
    s.update_transforms();
    // root rotates -Z into -X: child at (10-2, 0, 0); grandchild local +X scaled by 2 → -Z*2.
    const Vec3 cp = s.world().get<WorldTransform>(child)->matrix.transform_point({});
    const Vec3 gp = s.world().get<WorldTransform>(grandchild)->matrix.transform_point({});
    CHECK(near(cp, {8, 0, 0}));
    CHECK(near(gp, {8, 0, -2}));
    CHECK(near(s.world_matrix(grandchild).transform_point({}), gp));

    // keep_world reparent leaves the grandchild where it is.
    CHECK(s.set_parent(grandchild, kNullEntity, true));
    s.update_transforms();
    CHECK(near(s.world().get<WorldTransform>(grandchild)->matrix.transform_point({}), {8, 0, -2}));
    CHECK(near(s.world().get<Transform>(grandchild)->scale, {2, 2, 2}));
}

TEST_CASE("scene: save/load round trip is lossless and stable") {
    Scene s;
    s.name = "Round Trip";
    const Entity cam = s.create("Camera");
    s.world().add<CameraComponent>(cam).fov_y_degrees = 50.0f;
    s.world().get<Transform>(cam)->position = {0, 2, 5};
    const Entity sun = s.create("Sun");
    auto& light = s.world().add<LightComponent>(sun);
    light.intensity = 4.5f;
    light.color = {1.0f, 0.5f, 0.25f, 1.0f};
    const Entity group = s.create("Group");
    const Entity cube = s.create("Cube", group);
    auto& mr = s.world().add<MeshRendererComponent>(cube);
    mr.primitive = "cube";
    mr.roughness = 0.25f;
    s.world().get<Transform>(cube)->rotation = Quat::euler(0.1f, 0.2f, 0.3f);
    const Entity sprite = s.create("Sprite", group);
    auto& sp = s.world().add<SpriteComponent>(sprite);
    sp.texture_asset = Uuid::generate();
    sp.layer = -3;
    sp.flip_x = true;

    const json::Value doc = s.save();
    const std::string text = json::stringify(doc);
    Scene loaded;
    REQUIRE(loaded.load(doc).ok());
    CHECK(loaded.name == "Round Trip");
    CHECK(loaded.size() == 5);
    CHECK(json::stringify(loaded.save()) == text); // byte-identical re-save

    const Entity lcube = loaded.find(s.uuid(cube));
    REQUIRE(!lcube.is_null());
    CHECK(loaded.uuid(loaded.parent(lcube)) == s.uuid(group));
    CHECK(loaded.world().get<MeshRendererComponent>(lcube)->roughness == doctest::Approx(0.25f));
    const Quat q = loaded.world().get<Transform>(lcube)->rotation;
    const Quat o = s.world().get<Transform>(cube)->rotation;
    CHECK(std::abs(q.x * o.x + q.y * o.y + q.z * o.z + q.w * o.w) == doctest::Approx(1.0f).epsilon(1e-5));
    const Entity lsprite = loaded.find(s.uuid(sprite));
    CHECK(loaded.world().get<SpriteComponent>(lsprite)->texture_asset == sp.texture_asset);
    CHECK(loaded.world().get<SpriteComponent>(lsprite)->layer == -3);
    CHECK(loaded.world().get<LightComponent>(loaded.find(s.uuid(sun)))->color.g == doctest::Approx(0.5f));
    CHECK(loaded.world().get<WorldTransform>(lcube) != nullptr);

    const auto path = std::filesystem::temp_directory_path() / "victus_scene_test.vscene";
    REQUIRE(s.save_file(path).ok());
    Scene from_file;
    REQUIRE(from_file.load_file(path).ok());
    CHECK(json::stringify(from_file.save()) == text);
    std::filesystem::remove(path);
}

TEST_CASE("scene: unknown components survive load/save; invalid files are rejected") {
    const char* text = R"({
      "format": "victus.scene", "version": 1, "name": "x",
      "entities": [
        {"uuid": "5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11", "name": "child",
         "parent": "0f1e2d3c-4b5a-4968-8776-655443322110",
         "components": {"FuturePlugin.Thing": {"power": 9001}, "Transform": {"position": [1, 2, 3]}}},
        {"uuid": "0f1e2d3c-4b5a-4968-8776-655443322110", "name": "parent"}
      ]})";
    auto doc = json::parse(text);
    REQUIRE(doc.ok());
    Scene s;
    REQUIRE(s.load(*doc).ok());
    Uuid child_id;
    REQUIRE(Uuid::parse("5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11", child_id));
    const Entity child = s.find(child_id);
    REQUIRE(!child.is_null());
    CHECK(s.world().get<Name>(s.parent(child))->value == "parent");
    CHECK(s.world().get<Transform>(child)->position.z == 3.0f);
    const std::string resaved = json::stringify(s.save());
    CHECK(resaved.find("FuturePlugin.Thing") != std::string::npos);
    CHECK(resaved.find("9001") != std::string::npos);

    auto expect_fail = [](const char* bad_text) {
        auto d = json::parse(bad_text);
        REQUIRE(d.ok());
        Scene t;
        CHECK_FALSE(t.load(*d).ok());
        CHECK(t.size() == 0);
    };
    expect_fail(R"({"format":"other","version":1,"entities":[]})");
    expect_fail(R"({"format":"victus.scene","version":99,"entities":[]})");
    expect_fail(R"({"format":"victus.scene","version":1,"entities":[{"name":"no uuid"}]})");
    expect_fail(R"({"format":"victus.scene","version":1,"entities":[
        {"uuid":"5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11"},{"uuid":"5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11"}]})");
    expect_fail(R"({"format":"victus.scene","version":1,"entities":[
        {"uuid":"5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11","parent":"0f1e2d3c-4b5a-4968-8776-655443322110"}]})");
    expect_fail(R"({"format":"victus.scene","version":1,"entities":[
        {"uuid":"5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11","components":{"Transform":{"position":[1,"x",3]}}}]})");
    expect_fail(R"({"format":"victus.scene","version":1,"entities":[
        {"uuid":"5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11","parent":"0f1e2d3c-4b5a-4968-8776-655443322110"},
        {"uuid":"0f1e2d3c-4b5a-4968-8776-655443322110","parent":"5b0c2a52-8d1e-4f7a-9a51-3c2b1d0e9f11"}]})");
}

TEST_CASE("scene: duplicate copies a subtree with fresh identities") {
    Scene s;
    const Entity a = s.create("A");
    const Entity b = s.create("B", a);
    s.create("C", b);
    s.world().add<MeshRendererComponent>(b).primitive = "sphere";
    const Entity copy = s.duplicate(a);
    REQUIRE(!copy.is_null());
    CHECK(s.size() == 6);
    CHECK(s.uuid(copy) != s.uuid(a));
    CHECK(names(s, s.roots()) == std::vector<std::string>{"A", "A"});
    const auto kids = s.children(copy);
    REQUIRE(kids.size() == 1);
    CHECK(s.world().get<MeshRendererComponent>(kids[0])->primitive == "sphere");
    CHECK(names(s, s.children(kids[0])) == std::vector<std::string>{"C"});
    CHECK(s.children(b).size() == 1); // original untouched
}
