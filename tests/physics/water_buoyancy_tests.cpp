// SPDX-License-Identifier: Apache-2.0
// Water buoyancy (ADR-0095): Jolt submerged-volume buoyancy against Gerstner water volumes,
// physical fluid density, currents, enter/exit events and the per-body water contact.
#include "victus/gameplay/framework.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/water.hpp"
#include "victus/physics/physics.hpp"

#include <doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace victus;
using namespace victus::physics;
using scene::Scene;
using scene::Transform;

namespace {

constexpr float kDt = 1.0f / 60.0f;

std::unique_ptr<PhysicsWorld> make_world() {
    auto w = PhysicsWorld::create({});
    REQUIRE(w.ok());
    return std::move(w.value());
}

Entity pool(Scene& scene, float wave_height = 0.0f, math::Vec2 current = {0, 0}) {
    gameplay::register_components();
    const Entity e = scene.create("Pool");
    gameplay::WaterComponent w;
    w.size = {40, 40};
    w.wave_height = wave_height;
    w.current = current;
    scene.world().add<gameplay::WaterComponent>(e, w);
    return e;
}

// A 1 m cube of `mass` kg (density = mass kg/m^3), or a box of `size`.
Entity crate(Scene& scene, float mass, math::Vec3 at, math::Vec3 size = {1, 1, 1}) {
    const Entity e = scene.create("Crate");
    scene.world().get<Transform>(e)->position = at;
    ColliderComponent c;
    c.size = size;
    scene.world().add<ColliderComponent>(e, c);
    RigidBodyComponent rb;
    rb.mass = mass;
    scene.world().add<RigidBodyComponent>(e, rb);
    return e;
}

} // namespace

TEST_CASE("buoyancy: a half-density crate floats half submerged; a dense one sinks") {
    Scene scene;
    const Entity water = pool(scene);
    const Entity light = crate(scene, 500.0f, {-3, 2, 0});
    const Entity heavy = crate(scene, 3000.0f, {3, 2, 0});
    auto world = make_world();
    for (int i = 0; i < 600; ++i) world->step(scene, kDt);
    const float y = scene.world().get<Transform>(light)->position.y;
    CHECK(std::abs(y) < 0.08f);
    const WaterContact c = world->water_contact(light);
    CHECK(c.water == water);
    CHECK(c.submerged == doctest::Approx(0.5f).epsilon(0.15));
    CHECK(std::abs(c.surface) < 1e-4f);
    CHECK(scene.world().get<Transform>(heavy)->position.y < -5.0f);
    CHECK(world->water_contact(heavy).submerged == doctest::Approx(1.0f));
}

TEST_CASE("buoyancy: lighter rafts ride higher, denser water lifts more") {
    // A 2 x 0.5 x 2 m raft (2 m^3) is stable upright at any density (an upright cube is not
    // between ~21% and ~79% density and correctly tips onto an edge). Archimedes: the draft is
    // 0.5 m * density_body / density_fluid.
    auto rest = [](float density_body, float density) {
        Scene scene;
        const Entity w = pool(scene);
        scene.world().get<gameplay::WaterComponent>(w)->density = density;
        const Entity e = crate(scene, density_body * 2.0f, {0, 1, 0}, {2, 0.5f, 2});
        auto world = make_world();
        for (int i = 0; i < 600; ++i) world->step(scene, kDt);
        return scene.world().get<Transform>(e)->position.y;
    };
    const float y250 = rest(250.0f, 1000.0f), y750 = rest(750.0f, 1000.0f);
    CHECK(std::abs(y250 - 0.125f) < 0.02f); // draft 0.125 m: centre 0.25 - 0.125 above the surface
    CHECK(std::abs(y750 + 0.125f) < 0.02f);
    CHECK(std::abs(rest(500.0f, 2000.0f) - 0.125f) < 0.02f); // same draft as 250 in fresh water
}

TEST_CASE("buoyancy: the current carries floating bodies; waves move them") {
    Scene scene;
    (void)pool(scene, 0.0f, {1.5f, 0});
    const Entity e = crate(scene, 500.0f, {0, 0.2f, 0});
    auto world = make_world();
    for (int i = 0; i < 180; ++i) world->step(scene, kDt);
    CHECK(scene.world().get<Transform>(e)->position.x > 1.0f);

    Scene wavy;
    (void)pool(wavy, 1.0f);
    const Entity f = crate(wavy, 500.0f, {0, 0.0f, 0});
    auto w2 = make_world();
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 600; ++i) {
        gameplay::advance_water_time(wavy, kDt);
        w2->step(wavy, kDt);
        if (i > 120) {
            const float y = wavy.world().get<Transform>(f)->position.y;
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
    }
    CHECK(hi - lo > 0.2f); // bobs with the swell
    CHECK(hi < 1.5f);
    CHECK(lo > -1.5f);
}

TEST_CASE("buoyancy: water_entered / water_exited events and outside bodies") {
    Scene scene;
    const Entity water = pool(scene);
    scene.world().get<gameplay::WaterComponent>(water)->size = {4, 4};
    const Entity e = crate(scene, 500.0f, {0, 1.5f, 0});
    const Entity dry = crate(scene, 500.0f, {10, 1.5f, 0}); // outside the 4 x 4 pool: falls freely
    std::vector<std::string> seen;
    (void)gameplay::listen(scene, "", [&](Scene&, const gameplay::GameplayEvent& ev) {
        if (ev.name.rfind("water_", 0) == 0) {
            CHECK(ev.sender == water);
            CHECK(ev.target == e);
            seen.push_back(ev.name);
        }
    });
    auto world = make_world();
    for (int i = 0; i < 120; ++i) world->step(scene, kDt);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == "water_entered");
    CHECK(world->water_contact(dry).water.is_null());
    CHECK(scene.world().get<Transform>(dry)->position.y < -10.0f);

    // Teleport out of the pool: exit fires once.
    scene.world().get<Transform>(e)->position = {0, 10, 0};
    world->set_linear_velocity(e, {0, 0, 0});
    for (int i = 0; i < 3; ++i) world->step(scene, kDt);
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == "water_exited");
    CHECK(world->water_contact(e).water.is_null());
}
