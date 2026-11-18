// SPDX-License-Identifier: Apache-2.0
// 2D polygon and chain colliders: convex decomposition, concave static polygons, dynamic polygons,
// one-sided chains (open, loops, inside loops, mirroring), fallbacks, invalid outlines, reflection
// and debug lines.
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <cmath>
#include <numbers>

using namespace victus;
using namespace victus::physics;
using scene::Scene;
using scene::Transform;
using Shape2D = Collider2DComponent::Shape;

namespace {

std::unique_ptr<PhysicsWorld> make_world() {
    auto w = PhysicsWorld::create();
    REQUIRE(w.ok());
    return std::move(w.value());
}

void run(PhysicsWorld& w, Scene& scene, int steps) {
    for (int i = 0; i < steps; ++i) w.step(scene, 1.0f / 60.0f);
}

float area(const std::vector<math::Vec2>& p) { return std::fabs(signed_area2(p)) * 0.5f; }

bool convex_ccw(const std::vector<math::Vec2>& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const math::Vec2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
        if ((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x) < -1e-5f) return false;
    }
    return signed_area2(p) > 0;
}

Entity outline(Scene& scene, Shape2D shape, std::vector<math::Vec2> points, math::Vec3 pos = {}, bool loop = true) {
    const Entity e = scene.create("Outline");
    scene.world().get<Transform>(e)->position = pos;
    Collider2DComponent c;
    c.shape = shape;
    c.points = std::move(points);
    c.loop = loop;
    scene.world().add<Collider2DComponent>(e, c);
    return e;
}

Entity box_2d(Scene& scene, math::Vec3 p, math::Vec2 size = {1, 1}) {
    const Entity e = scene.create("Box2D");
    scene.world().get<Transform>(e)->position = p;
    Collider2DComponent c;
    c.size = size;
    scene.world().add<Collider2DComponent>(e, c);
    scene.world().add<RigidBody2DComponent>(e, RigidBody2DComponent{});
    return e;
}

float y_of(Scene& scene, Entity e) { return scene.world().get<Transform>(e)->position.y; }

// A cup: outer walls x in [-3, 3], inner floor at y = 1, rim at y = 4 (concave, 8 points).
std::vector<math::Vec2> cup() { return {{-3, 0}, {3, 0}, {3, 4}, {2, 4}, {2, 1}, {-2, 1}, {-2, 4}, {-3, 4}}; }

} // namespace

TEST_CASE("decompose_polygon splits concave outlines into small convex pieces with the same area") {
    auto square = decompose_polygon({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    REQUIRE(square.ok());
    CHECK(square->size() == 1);

    // Clockwise L-shape with a repeated point and a collinear point.
    const std::vector<math::Vec2> l = {{0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}, {2, 0.5f}, {2, 0}, {2, 0}};
    auto pieces = decompose_polygon(l);
    REQUIRE(pieces.ok());
    CHECK(pieces->size() == 2);
    float total = 0;
    for (const auto& p : *pieces) {
        CHECK(convex_ccw(p));
        total += area(p);
    }
    CHECK(total == doctest::Approx(3.0f));

    // A 40-point comb: every piece convex with at most 8 points, areas add up.
    std::vector<math::Vec2> comb = {{0, 0}, {20, 0}};
    for (int i = 19; i >= 0; --i) {
        comb.push_back({static_cast<float>(i) + 1.0f, 3});
        comb.push_back({static_cast<float>(i) + 0.5f, 1});
    }
    pieces = decompose_polygon(comb);
    REQUIRE(pieces.ok());
    total = 0;
    for (const auto& p : *pieces) {
        CHECK(convex_ccw(p));
        CHECK(p.size() <= 8);
        total += area(p);
    }
    CHECK(total == doctest::Approx(area(comb)).epsilon(1e-4));
    CHECK(pieces->size() < comb.size() - 2); // merged beyond plain triangles

    CHECK_FALSE(decompose_polygon({{0, 0}, {1, 1}, {1, 0}, {0, 1}}).ok()); // bow tie crosses itself
    CHECK_FALSE(decompose_polygon({{0, 0}, {1, 0}, {2, 0}}).ok());          // collinear
    CHECK_FALSE(decompose_polygon({{0, 0}, {1, 0}}).ok());
    CHECK_FALSE(decompose_polygon({{0, 0}, {NAN, 0}, {0, 1}}).ok());
    CHECK_FALSE(decompose_polygon(std::vector<math::Vec2>(kMaxPolygonPoints + 1, {0, 0})).ok());
    CHECK(self_intersects({{0, 0}, {1, 1}, {1, 0}, {0, 1}}, true));
    CHECK(self_intersects({{0, 0}, {1, 1}, {1, 0}, {0, 1}}, false)); // the open zigzag still crosses
    CHECK_FALSE(self_intersects({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, true));
}

TEST_CASE("a static concave polygon keeps its hollow: a box falls into the cup") {
    Scene scene;
    auto w = make_world();
    const Entity c = outline(scene, Shape2D::Polygon, cup());
    const Entity box = box_2d(scene, {0, 6, 0});
    run(*w, scene, 180);
    REQUIRE(w->has_body(c));
    CHECK(y_of(scene, box) == doctest::Approx(1.5f).epsilon(0.03));
    const auto hit = w->raycast_2d({0, 10}, {0, -1}, 20, kAllLayers, box);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == c);
    CHECK(hit->point.y == doctest::Approx(1.0f).epsilon(0.01));
    const auto rim = w->raycast_2d({2.5f, 10}, {0, -1}, 20);
    REQUIRE(rim.has_value());
    CHECK(rim->point.y == doctest::Approx(4.0f).epsilon(0.01));
}

TEST_CASE("dynamic polygons fall, rest on their outline and take the RigidBody2D mass") {
    Scene scene;
    auto w = make_world();
    const Entity ground = outline(scene, Shape2D::Box, {}, {0, -0.5f, 0});
    scene.world().get<Collider2DComponent>(ground)->size = {20, 1};
    const Entity tri = outline(scene, Shape2D::Polygon, {{-1, 0}, {1, 0}, {0, 1.5f}}, {0, 3, 0});
    RigidBody2DComponent rb;
    rb.mass = 4;
    scene.world().add<RigidBody2DComponent>(tri, rb);
    run(*w, scene, 240);
    CHECK(y_of(scene, tri) == doctest::Approx(0.0f).epsilon(0.03));
    CHECK(std::fabs(w->linear_velocity(tri).y) < 0.05f);
    w->add_impulse(tri, {0, 8, 0});
    CHECK(w->linear_velocity(tri).y == doctest::Approx(2.0f).epsilon(0.02)); // 8 N·s / 4 kg
}

TEST_CASE("open chains are solid on the left of their direction, also when mirrored") {
    for (const float mirror : {1.0f, -1.0f}) {
        CAPTURE(mirror);
        Scene scene;
        auto w = make_world();
        // Ground drawn left to right with a bump; and a second ground drawn right to left.
        const Entity ground = outline(scene, Shape2D::Chain, {{-10, 0}, {-1, 0}, {0, 0.5f}, {1, 0}, {10, 0}}, {}, false);
        scene.world().get<Transform>(ground)->scale = {mirror, 1, 1};
        const Entity ceiling = outline(scene, Shape2D::Chain, {{30, 0}, {10, 0}}, {0, 0, 0}, false);
        scene.world().get<Transform>(ceiling)->position = {0, -20, 0};
        const Entity on_ground = box_2d(scene, {-5, 3, 0});
        const Entity on_bump = box_2d(scene, {0, 3, 0}, {0.4f, 0.4f});
        const Entity through = box_2d(scene, {20, -17, 0});
        run(*w, scene, 180);
        CHECK(w->has_body(ground));
        CHECK(y_of(scene, on_ground) == doctest::Approx(0.5f).epsilon(0.03));
        CHECK(y_of(scene, on_bump) > 0.3f);               // rests on or slides off the bump, never through
        CHECK(y_of(scene, through) < -21.0f);             // falls through the back side of a right-to-left chain
        const auto hit = w->raycast_2d({-5, 5}, {0, -1}, 20, kAllLayers, on_ground);
        REQUIRE(hit.has_value());
        CHECK(hit->entity == ground);
        CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(0.01));
    }
}

TEST_CASE("chain loops collide outside, or inside for arenas, in either winding") {
    const std::vector<math::Vec2> ccw = {{-4, -4}, {4, -4}, {4, 4}, {-4, 4}};
    const std::vector<math::Vec2> cw(ccw.rbegin(), ccw.rend());
    for (const auto* pts : {&ccw, &cw}) {
        CAPTURE(pts == &ccw);
        Scene scene;
        auto w = make_world();
        outline(scene, Shape2D::Chain, *pts); // outside: a box lands on top at y = 4
        const Entity top = box_2d(scene, {0, 7, 0});
        const Entity arena = outline(scene, Shape2D::Chain, *pts, {20, 0, 0});
        scene.world().get<Collider2DComponent>(arena)->inside = true; // inside: a box inside lands on the floor
        const Entity inner = box_2d(scene, {20, 2, 0});
        run(*w, scene, 180);
        CHECK(y_of(scene, top) == doctest::Approx(4.5f).epsilon(0.03));
        CHECK(y_of(scene, inner) == doctest::Approx(-3.5f).epsilon(0.03));
    }
}

TEST_CASE("chains on dynamic bodies or triggers fill their outline; invalid outlines create no body") {
    Scene scene;
    auto w = make_world();
    const Entity ground = outline(scene, Shape2D::Box, {}, {0, -0.5f, 0});
    scene.world().get<Collider2DComponent>(ground)->size = {20, 1};
    const Entity crate = outline(scene, Shape2D::Chain, {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}}, {0, 3, 0});
    scene.world().add<RigidBody2DComponent>(crate, RigidBody2DComponent{});
    const Entity zone = outline(scene, Shape2D::Chain, {{-1, 0}, {1, 0}, {1, 2}, {-1, 2}}, {5, 0, 0});
    scene.world().get<Collider2DComponent>(zone)->is_trigger = true;
    const Entity bowtie = outline(scene, Shape2D::Polygon, {{0, 0}, {1, 1}, {1, 0}, {0, 1}}, {-5, 0, 0});
    const Entity lonely = outline(scene, Shape2D::Chain, {{0, 0}}, {-8, 0, 0}, false);
    run(*w, scene, 180);
    CHECK(y_of(scene, crate) == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(w->has_body(zone)); // a sensor polygon
    CHECK_FALSE(w->has_body(bowtie));
    CHECK_FALSE(w->has_body(lonely));
    CHECK(w->stats().bodies_2d == 3);
    // Fixing the outline builds the body on the next step.
    scene.world().get<Collider2DComponent>(bowtie)->points = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    run(*w, scene, 1);
    CHECK(w->has_body(bowtie));
}

TEST_CASE("polygon and chain colliders reflect, round-trip and draw outlines with side ticks") {
    register_components();
    Scene scene;
    const Entity e = outline(scene, Shape2D::Chain, {{0, 0}, {1, 0}, {2, 1}}, {}, false);
    scene.world().get<Collider2DComponent>(e)->inside = true;
    Scene loaded;
    REQUIRE(loaded.load(scene.save()).ok());
    Entity le;
    loaded.world().each<Collider2DComponent>([&](Entity x, Collider2DComponent&) { le = x; });
    REQUIRE_FALSE(le.is_null());
    CHECK(*loaded.world().get<Collider2DComponent>(le) == *scene.world().get<Collider2DComponent>(e));

    const auto* schema = scene::find_schema("Collider2D");
    REQUIRE(schema);
    Collider2DComponent tmp;
    for (const char* bad : {R"({"points": [[0, 0], [1]]})", R"({"points": 3})", R"({"points": [[0, "x"]]})"}) {
        CAPTURE(bad);
        auto doc = json::parse(bad);
        REQUIRE(doc.ok());
        CHECK_FALSE(schema->load(&tmp, doc.value()).ok());
    }
    json::Value many = json::Value::array();
    for (std::size_t i = 0; i <= scene::kMaxListLength; ++i) many.push(json::parse("[0, 0]").value());
    json::Value obj = json::Value::object();
    obj.set("points", many);
    CHECK_FALSE(schema->load(&tmp, obj).ok());

    std::vector<DebugLine> lines;
    collider_lines(scene, lines);
    CHECK(lines.size() == 2 + 2); // two segments, one side tick each
    lines.clear();
    scene.world().get<Collider2DComponent>(e)->shape = Shape2D::Polygon;
    collider_lines(scene, lines);
    CHECK(lines.size() == 3); // closed triangle
}
