// SPDX-License-Identifier: Apache-2.0
// Wheeled vehicle tests (ADR-0166, GAP-24): a four-wheel car built from scene entities (chassis
// RigidBody + Collider, VehicleWheel children), simulated with the real Jolt backend through the
// gameplay phases at 60 Hz.
#include "victus/physics/physics.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <cmath>

using namespace victus;
using namespace victus::physics;
using scene::Scene;
using scene::Transform;

namespace {

constexpr float kVehicleDt = 1.0f / 60.0f;

struct VehicleSim {
    std::unique_ptr<PhysicsWorld> world;
    gameplay::Simulation sim{kVehicleDt};
    Scene scene;
    VehicleSim() {
        auto w = PhysicsWorld::create({});
        REQUIRE(w.ok());
        world = std::move(w.value());
        add_physics_systems(sim, *world);
        const Entity g = scene.create("Ground");
        scene.world().get<Transform>(g)->position = {0, -0.5f, 0};
        ColliderComponent c;
        c.size = {400, 1, 400};
        scene.world().add<ColliderComponent>(g, c);
    }
    void run(float seconds) {
        for (int i = 0; i < static_cast<int>(seconds / kVehicleDt + 0.5f); ++i) sim.tick(scene, kVehicleDt);
    }
    math::Vec3 pos(Entity e) { return scene.world_matrix(e).transform_point({}); }
};

struct Car {
    Entity chassis;
    Entity wheels[4]; // front-left, front-right, rear-left, rear-right
};

Car car(Scene& s, math::Vec3 at = {0, 1.2f, 0}, bool add_wheels = true) {
    Car c;
    c.chassis = s.create("Car");
    s.world().get<Transform>(c.chassis)->position = at;
    ColliderComponent col;
    col.size = {1.8f, 0.6f, 4.0f};
    s.world().add<ColliderComponent>(c.chassis, col);
    RigidBodyComponent rb;
    rb.mass = 1500;
    s.world().add<RigidBodyComponent>(c.chassis, rb);
    s.world().add<VehicleComponent>(c.chassis);
    if (!add_wheels) return c;
    const math::Vec3 at_wheel[4] = {{0.9f, -0.3f, 1.4f}, {-0.9f, -0.3f, 1.4f}, {0.9f, -0.3f, -1.4f}, {-0.9f, -0.3f, -1.4f}};
    for (int i = 0; i < 4; ++i) {
        c.wheels[i] = s.create(i < 2 ? "FrontWheel" : "RearWheel", c.chassis);
        s.world().get<Transform>(c.wheels[i])->position = at_wheel[i];
        VehicleWheelComponent w;
        w.steer_degrees = i < 2 ? 30.0f : 0.0f;
        w.driven = i >= 2; // rear-wheel drive
        w.handbrake_torque = i >= 2 ? 4000.0f : 0.0f;
        s.world().add<VehicleWheelComponent>(c.wheels[i], w);
    }
    return c;
}

} // namespace

TEST_CASE("vehicle: a four-wheel car settles on its suspension (ADR-0166)") {
    register_components();
    VehicleSim v;
    const Car c = car(v.scene);
    v.run(3.0f);
    const VehicleState st = v.world->vehicle_state(c.chassis);
    REQUIRE(st.valid);
    CHECK(st.error.empty());
    REQUIRE(st.wheels.size() == 4);
    CHECK(st.wheels_in_contact == 4);
    CHECK(std::abs(st.speed) < 0.05f);
    // Wheel centres sit one radius above the ground; the chassis rests above them.
    for (const Entity w : c.wheels) CHECK(v.pos(w).y == doctest::Approx(0.35f).epsilon(0.08));
    // Attachment 0.1 m above the chassis origin (rest centre -0.3 plus 0.4 suspension), so the
    // chassis sits at wheel centre - 0.1 + current suspension length.
    const float y = v.pos(c.chassis).y;
    CHECK(y == doctest::Approx(0.35f - 0.1f + st.wheels[0].suspension).epsilon(0.02));
    for (const auto& w : st.wheels) {
        CHECK(w.contact);
        CHECK(w.suspension > 0.14f); // between min (0.15) and max (0.4) length
        CHECK(w.suspension < 0.41f);
    }
    // The chassis stays level: no roll or pitch from a symmetric car.
    const math::Mat4 m = v.scene.world_matrix(c.chassis);
    const math::Vec3 chassis_up = m.transform_point({0, 1, 0}) - m.transform_point({});
    CHECK(chassis_up.y > 0.999f);
}

TEST_CASE("vehicle: throttle accelerates, the gearbox shifts and the wheels spin") {
    register_components();
    VehicleSim v;
    const Car c = car(v.scene);
    // All-wheel drive: 500 N*m through two rear wheels spins them, and Jolt's gearbox does not
    // shift up while a driven wheel slips.
    for (int i = 0; i < 2; ++i) v.scene.world().get<VehicleWheelComponent>(c.wheels[i])->driven = true;
    v.run(1.0f);
    const float z0 = v.pos(c.chassis).z;
    v.world->set_vehicle_input(c.chassis, {.forward = 1.0f});
    CHECK(v.world->vehicle_input(c.chassis).forward == 1.0f);
    v.run(5.0f);
    const VehicleState st = v.world->vehicle_state(c.chassis);
    REQUIRE(st.valid);
    MESSAGE("speed after 5 s full throttle: " << st.speed << " m/s, gear " << st.gear << ", rpm " << st.engine_rpm);
    CHECK(st.speed > 8.0f);
    CHECK(st.gear >= 2);
    CHECK(st.engine_rpm > 1000.0f);
    CHECK(v.pos(c.chassis).z - z0 > 15.0f);
    CHECK(std::abs(v.pos(c.chassis).x) < 1.0f); // straight line
    for (const auto& w : st.wheels) CHECK(w.rpm > 50.0f);
    // Wheel transforms follow the simulation: the rear wheels have rotated about their axle.
    const math::Mat4 wm = v.scene.world_matrix(c.wheels[2]);
    const math::Vec3 wheel_up = wm.transform_point({0, 1, 0}) - wm.transform_point({});
    CHECK(wheel_up.y < 0.999f);
}

TEST_CASE("vehicle: steering turns, braking stops and reverse backs up") {
    register_components();
    SUBCASE("steer right") {
        VehicleSim v;
        const Car c = car(v.scene);
        v.run(1.0f);
        v.world->set_vehicle_input(c.chassis, {.forward = 0.6f, .steer = 1.0f});
        v.run(1.0f);
        const auto st = v.world->vehicle_state(c.chassis);
        CHECK(st.wheels[0].steer_degrees != doctest::Approx(0.0f));
        CHECK(st.wheels[2].steer_degrees == doctest::Approx(0.0f)); // rear wheels do not steer
        v.run(3.0f);
        // +X is the car's left: a right turn moves it toward -X.
        CHECK(v.pos(c.chassis).x < -2.0f);
    }
    SUBCASE("brake") {
        VehicleSim v;
        const Car c = car(v.scene);
        v.run(1.0f);
        v.world->set_vehicle_input(c.chassis, {.forward = 1.0f});
        v.run(4.0f);
        REQUIRE(v.world->vehicle_state(c.chassis).speed > 6.0f);
        v.world->set_vehicle_input(c.chassis, {.brake = 1.0f});
        v.run(4.0f);
        CHECK(std::abs(v.world->vehicle_state(c.chassis).speed) < 0.3f);
    }
    SUBCASE("reverse") {
        VehicleSim v;
        const Car c = car(v.scene);
        v.run(1.0f);
        v.world->set_vehicle_input(c.chassis, {.forward = -1.0f});
        v.run(3.0f);
        const auto st = v.world->vehicle_state(c.chassis);
        CHECK(st.speed < -1.0f);
        CHECK(st.gear < 0);
        CHECK(v.pos(c.chassis).z < -2.0f);
    }
}

TEST_CASE("vehicle: player_input drives from the keyboard") {
    register_components();
    VehicleSim v;
    gameplay::ManualInput input;
    v.sim.input = &input;
    const Car c = car(v.scene);
    v.scene.world().get<VehicleComponent>(c.chassis)->player_input = true;
    v.run(1.0f);
    input.set_key("W", true);
    v.run(3.0f);
    CHECK(v.world->vehicle_state(c.chassis).speed > 4.0f);
    // S while rolling forward brakes before it reverses.
    input.set_key("W", false);
    input.set_key("S", true);
    v.sim.tick(v.scene, kVehicleDt);
    CHECK(v.world->vehicle_input(c.chassis).brake == 1.0f);
    CHECK(v.world->vehicle_input(c.chassis).forward == 0.0f);
}

TEST_CASE("vehicle: invalid setups report why and are not simulated") {
    register_components();
    VehicleSim v;
    const Car no_wheels = car(v.scene, {10, 1.2f, 0}, false);
    Car fixed = car(v.scene, {-10, 1.2f, 0});
    v.scene.world().get<RigidBodyComponent>(fixed.chassis)->type = BodyType::Static;
    v.run(0.5f);
    const auto a = v.world->vehicle_state(no_wheels.chassis);
    CHECK_FALSE(a.valid);
    CHECK(a.error.find("VehicleWheel") != std::string::npos);
    const auto b = v.world->vehicle_state(fixed.chassis);
    CHECK_FALSE(b.valid);
    CHECK(b.error.find("dynamic") != std::string::npos);
    CHECK_FALSE(v.world->vehicle_state(v.scene.create("Nothing")).valid);
    // Fixing the setup starts the simulation; removing the component stops it.
    v.scene.world().get<RigidBodyComponent>(fixed.chassis)->type = BodyType::Dynamic;
    v.run(1.0f);
    CHECK(v.world->vehicle_state(fixed.chassis).valid);
    v.scene.world().remove<VehicleComponent>(fixed.chassis);
    v.run(0.1f);
    CHECK_FALSE(v.world->vehicle_state(fixed.chassis).valid);
}

TEST_CASE("vehicle: components are reflected for scenes and the inspector") {
    register_components();
    const auto* vc = scene::find_schema("Vehicle");
    REQUIRE(vc != nullptr);
    const auto* wc = scene::find_schema("VehicleWheel");
    REQUIRE(wc != nullptr);
}

TEST_CASE("vehicle: make_vehicle turns a box or a model with wheel nodes into a drivable car") {
    register_components();
    SUBCASE("a plain box gets four wheels and drives") {
        VehicleSim v;
        const Entity box = v.scene.create("Box");
        v.scene.world().get<Transform>(box)->position = {0, 1.0f, 0};
        ColliderComponent col;
        col.size = {1.8f, 0.6f, 4.0f};
        v.scene.world().add<ColliderComponent>(box, col);
        CHECK(make_vehicle(v.scene, box) == 4);
        REQUIRE(v.scene.world().get<RigidBodyComponent>(box));
        CHECK(v.scene.world().get<RigidBodyComponent>(box)->type == BodyType::Dynamic);
        CHECK(v.scene.children(box).size() == 4);
        CHECK(make_vehicle(v.scene, box) == 4); // idempotent
        CHECK(v.scene.children(box).size() == 4);
        v.run(1.0f);
        v.world->set_vehicle_input(box, {.forward = 1.0f});
        v.run(2.0f);
        const auto st = v.world->vehicle_state(box);
        REQUIRE(st.valid);
        CHECK(st.wheels_in_contact == 4);
        CHECK(st.speed > 3.0f);
    }
    SUBCASE("named wheel nodes are reused") {
        Scene s;
        const Entity body = s.create("Truck");
        for (const char* n : {"FrontWheel_L", "front_tire_r", "RearTyre.L", "rear wheel R", "Mirror"}) {
            const Entity c = s.create(n, body);
            s.world().get<Transform>(c)->position = {0, -0.3f, n[0] == 'F' || n[0] == 'f' ? 1.5f : -1.5f};
        }
        CHECK(make_vehicle(s, body) == 4);
        CHECK(s.world().get<ColliderComponent>(body)); // a default box was added
        const auto* front = s.world().get<VehicleWheelComponent>(s.find_by_name("front_tire_r"));
        const auto* rear = s.world().get<VehicleWheelComponent>(s.find_by_name("RearTyre.L"));
        REQUIRE(front);
        REQUIRE(rear);
        CHECK(front->steer_degrees > 0);
        CHECK(rear->steer_degrees == 0);
        CHECK(rear->handbrake_torque > 0);
        CHECK(s.world().get<VehicleWheelComponent>(s.find_by_name("Mirror")) == nullptr);
    }
    CHECK(make_vehicle(*std::make_unique<Scene>(), Entity{}) == 0);
}
