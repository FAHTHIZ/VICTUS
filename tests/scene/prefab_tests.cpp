// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/scene/prefab.hpp"

#include <filesystem>

using namespace victus;
using namespace victus::scene;
using math::Vec3;

namespace {
bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return math::length(a - b) < eps; }

// A "Turret": root with a light, a Barrel child (mesh) and a Muzzle grandchild whose Camera's
// unknown "target" field references the Barrel by uuid (stands in for joints/script references).
Entity make_turret(Scene& s) {
    const Entity root = s.create("Turret");
    s.world().get<Transform>(root)->position = {5, 0, 2};
    s.world().get<Transform>(root)->scale = {2, 2, 2};
    s.world().add<LightComponent>(root).intensity = 7.0f;
    const Entity barrel = s.create("Barrel", root);
    s.world().get<Transform>(barrel)->position = {0, 1, 0};
    s.world().add<MeshRendererComponent>(barrel).primitive = "cylinder";
    const Entity muzzle = s.create("Muzzle", barrel);
    auto& u = s.world().add<UnknownComponents>(muzzle);
    json::Value aim = json::Value::object();
    aim.set("target", s.uuid(barrel).to_string());
    u.components.emplace_back("Aim", aim);
    return root;
}
Entity child_named(const Scene& s, Entity parent, std::string_view name) {
    for (const Entity c : s.children(parent))
        if (s.world().get<Name>(c)->value == name) return c;
    return kNullEntity;
}
} // namespace

TEST_CASE("prefab: make, save, load, validate") {
    Scene s;
    const Entity root = make_turret(s);
    const Prefab p = make_prefab(s, root);
    CHECK(p.name == "Turret");
    REQUIRE(p.size() == 3);
    CHECK(p.root() == s.uuid(root));
    // Root placement belongs to instances; rotation/scale stay.
    CHECK(p.entities[0]["components"]["Transform"]["position"][0].as_float() == 0.0f);
    CHECK(p.entities[0]["components"]["Transform"]["scale"][0].as_float() == 2.0f);
    CHECK(p.entities[0].find("parent") == nullptr);

    const auto dir = std::filesystem::temp_directory_path() / "victus_prefab_tests";
    std::filesystem::remove_all(dir);
    REQUIRE(save_prefab_file(p, dir / "turret.vprefab").ok());
    auto loaded = load_prefab_file(dir / "turret.vprefab");
    REQUIRE(loaded);
    CHECK(loaded->name == "Turret");
    CHECK(loaded->entities == p.entities);

    // Validation errors.
    json::Value doc = prefab_to_json(p);
    doc.set("format", "victus.scene");
    CHECK_FALSE(parse_prefab(doc));
    doc = prefab_to_json(p);
    doc.set("version", 99);
    auto r = parse_prefab(doc);
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("version 99") != std::string::npos);
    json::Value two_roots = prefab_to_json(p);
    json::Value ents = json::Value::array();
    ents.push(p.entities[0]);
    json::Value orphan = p.entities[1];
    orphan.set("parent", Uuid::generate().to_string());
    ents.push(orphan);
    two_roots.set("entities", ents);
    CHECK_FALSE(parse_prefab(two_roots));
    json::Value dup = prefab_to_json(p);
    json::Value ents2 = json::Value::array();
    ents2.push(p.entities[0]);
    json::Value d = p.entities[1];
    d.set("uuid", p.entities[0]["uuid"]);
    ents2.push(d);
    dup.set("entities", ents2);
    CHECK_FALSE(parse_prefab(dup));
    std::filesystem::remove_all(dir);
}

TEST_CASE("prefab: instantiate remaps references, derives stable uuids and round-trips scenes") {
    Scene src;
    const Prefab p = make_prefab(src, make_turret(src));
    const Uuid asset = Uuid::generate();

    Scene s;
    const Entity holder = s.create("Holder");
    InstantiateOptions o;
    o.parent = holder;
    o.position = Vec3{1, 2, 3};
    o.uuid = Uuid::generate();
    auto a = instantiate(s, p, asset, o);
    REQUIRE(a);
    CHECK(s.uuid(*a) == o.uuid);
    CHECK(s.parent(*a) == holder);
    CHECK(near(s.world().get<Transform>(*a)->position, {1, 2, 3}));
    CHECK(near(s.world().get<Transform>(*a)->scale, {2, 2, 2}));
    CHECK(s.world().get<LightComponent>(*a)->intensity == doctest::Approx(7.0f));
    REQUIRE(s.world().get<PrefabInstance>(*a));
    CHECK(s.world().get<PrefabInstance>(*a)->prefab == asset);
    const Entity barrel = child_named(s, *a, "Barrel");
    REQUIRE_FALSE(barrel.is_null());
    const Entity muzzle = child_named(s, barrel, "Muzzle");
    REQUIRE_FALSE(muzzle.is_null());
    Uuid barrel_local;
    REQUIRE(Uuid::parse(p.entities[1]["uuid"].as_string(), barrel_local));
    CHECK(s.uuid(barrel) == derive_part_uuid(o.uuid, barrel_local));
    // The reference inside the prefab now points at this instance's barrel.
    CHECK(s.save_entity(muzzle)["components"]["Aim"]["target"].as_string() == s.uuid(barrel).to_string());
    CHECK(instance_root(s, muzzle) == *a);
    CHECK(instance_root(s, holder).is_null());

    // Same root uuid again is refused; a second instance gets distinct uuids.
    CHECK_FALSE(instantiate(s, p, asset, o));
    auto b = instantiate(s, p, asset);
    REQUIRE(b);
    CHECK(s.uuid(child_named(s, *b, "Barrel")) != s.uuid(barrel));
    CHECK(near(s.world().get<Transform>(*b)->position, {0, 0, 0}));
    CHECK(find_instances(s).size() == 2);
    CHECK(find_instances(s, asset).size() == 2);
    CHECK(find_instances(s, Uuid::generate()).empty());

    // Deterministic: the same root uuid in another scene gives the same part uuids.
    Scene s2;
    InstantiateOptions o2;
    o2.uuid = o.uuid;
    auto c = instantiate(s2, p, asset, o2);
    REQUIRE(c);
    CHECK(s2.uuid(child_named(s2, *c, "Barrel")) == s.uuid(barrel));

    // Scene save/load keeps the instance link.
    Scene s3;
    REQUIRE(s3.load(s.save()).ok());
    const Entity a3 = s3.find(o.uuid);
    REQUIRE(s3.world().get<PrefabInstance>(a3));
    CHECK(s3.world().get<PrefabPart>(s3.find(s.uuid(barrel)))->source == s.world().get<PrefabPart>(barrel)->source);
}

TEST_CASE("prefab: updating instances keeps placement, overrides and added entities") {
    Scene src;
    const Entity src_root = make_turret(src);
    Prefab p = make_prefab(src, src_root);
    const Uuid asset = Uuid::generate();
    Scene s;
    auto a = instantiate(s, p, asset, {.position = Vec3{4, 0, 0}});
    auto b = instantiate(s, p, asset, {.position = Vec3{-4, 0, 0}});
    REQUIRE(a);
    REQUIRE(b);
    const Entity a_barrel = child_named(s, *a, "Barrel");
    const Uuid a_barrel_uuid = s.uuid(a_barrel);

    // Instance a: rename, recolour the barrel (marked as an override), add an extra child.
    s.world().get<Name>(*a)->value = "Left turret";
    s.world().get<MeshRendererComponent>(a_barrel)->base_color = {1, 0, 0, 1};
    CHECK(mark_override(s, a_barrel, "MeshRenderer", p));
    CHECK_FALSE(mark_override(s, a_barrel, "MeshRenderer", p)); // already recorded
    CHECK_FALSE(mark_override(s, *a, "Transform", p));          // root placement is always kept
    CHECK(is_overridden(s, a_barrel, "MeshRenderer", p));
    const Entity extra = s.create("Extra", a_barrel);

    // The prefab changes: brighter light, barrel moves, a Base part is added, Muzzle removed.
    src.world().get<LightComponent>(src_root)->intensity = 11.0f;
    const Entity src_barrel = child_named(src, src_root, "Barrel");
    src.world().get<Transform>(src_barrel)->position = {0, 3, 0};
    src.world().get<MeshRendererComponent>(src_barrel)->primitive = "cube";
    src.destroy(child_named(src, src_barrel, "Muzzle"));
    src.create("Base", src_root);
    p = make_prefab(src, src_root);

    auto n = update_instances(s, asset, p);
    REQUIRE(n);
    CHECK(*n == 2);
    for (const Entity e : {*a, *b}) {
        CHECK(s.world().get<LightComponent>(e)->intensity == doctest::Approx(11.0f));
        CHECK_FALSE(child_named(s, e, "Base").is_null());
        const Entity barrel = child_named(s, e, "Barrel");
        REQUIRE_FALSE(barrel.is_null());
        CHECK(child_named(s, barrel, "Muzzle").is_null());
        CHECK(near(s.world().get<Transform>(barrel)->position, {0, 3, 0}));
    }
    CHECK(near(s.world().get<Transform>(*a)->position, {4, 0, 0}));
    CHECK(near(s.world().get<Transform>(*b)->position, {-4, 0, 0}));
    CHECK(s.world().get<Name>(*a)->value == "Left turret");
    // The override kept the whole MeshRenderer of a's barrel (red cylinder); b took the change.
    const Entity a_barrel2 = child_named(s, *a, "Barrel");
    CHECK(s.uuid(a_barrel2) == a_barrel_uuid); // identity kept across updates
    CHECK(s.world().get<MeshRendererComponent>(a_barrel2)->base_color.g == doctest::Approx(0.0f));
    CHECK(s.world().get<MeshRendererComponent>(a_barrel2)->primitive == "cylinder");
    CHECK(s.world().get<MeshRendererComponent>(child_named(s, *b, "Barrel"))->primitive == "cube");
    CHECK(s.world().alive(extra));
    CHECK(s.parent(extra) == a_barrel2);

    // Revert drops the overrides.
    REQUIRE(update_instance(s, *a, p, /*keep_overrides=*/false).ok());
    CHECK(s.world().get<MeshRendererComponent>(child_named(s, *a, "Barrel"))->primitive == "cube");
    CHECK(s.world().get<PrefabInstance>(*a)->overrides.empty());
}

TEST_CASE("prefab: apply an instance back to the prefab and link existing entities") {
    Scene s;
    const Entity root = make_turret(s);
    const Uuid asset = Uuid::generate();
    Prefab p = make_prefab(s, root);
    link_as_instance(s, root, asset);
    REQUIRE(s.world().get<PrefabInstance>(root));
    const Entity barrel = child_named(s, root, "Barrel");
    CHECK(s.world().get<PrefabPart>(barrel)->source == s.uuid(barrel));
    CHECK(near(s.world().get<Transform>(root)->position, {5, 0, 2})); // the original stays in place

    auto other = instantiate(s, p, asset, {.position = Vec3{0, 0, 9}});
    REQUIRE(other);

    // Edit the original instance: add a Scope part under the barrel, change the light.
    const Entity scope = s.create("Scope", barrel);
    s.world().get<LightComponent>(root)->intensity = 2.0f;
    s.world().get<Transform>(root)->position = {100, 0, 0};
    const Prefab applied = apply_instance(s, root, p);
    CHECK(applied.size() == 4);
    CHECK(applied.root() == p.root());
    CHECK(s.world().get<PrefabPart>(scope)); // linked as a part
    // The prefab root keeps its previous placement (zero), not the instance's.
    CHECK(applied.entities[0]["components"]["Transform"]["position"][0].as_float() == 0.0f);

    auto n = update_instances(s, asset, applied);
    REQUIRE(n);
    CHECK(*n == 2);
    // The edited instance is unchanged (no duplicate Scope), the other one gained a Scope.
    CHECK(s.children(barrel).size() == 2);
    const Entity other_barrel = child_named(s, *other, "Barrel");
    CHECK_FALSE(child_named(s, other_barrel, "Scope").is_null());
    CHECK(s.world().get<LightComponent>(*other)->intensity == doctest::Approx(2.0f));
    CHECK(near(s.world().get<Transform>(*other)->position, {0, 0, 9}));
}
