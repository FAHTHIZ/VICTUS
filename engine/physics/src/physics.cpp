// SPDX-License-Identifier: Apache-2.0
#include "victus/physics/physics.hpp"

#include "internal.hpp"
#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace victus::physics {

namespace detail {

detail::Pose world_pose(const scene::Scene& scene, Entity e) {
    Pose p;
    math::decompose(scene.world_matrix(e), p.position, p.rotation, p.scale);
    p.rotation = math::normalize(p.rotation);
    return p;
}

void write_world_pose(scene::Scene& scene, Entity e, math::Vec3 position, math::Quat rotation) {
    auto* t = scene.world().get<scene::Transform>(e);
    if (!t) return;
    const Entity parent = scene.parent(e);
    if (parent.is_null() || !scene.world().alive(parent)) {
        t->position = position;
        t->rotation = math::normalize(rotation);
        return;
    }
    const math::Vec3 scale = t->scale;
    const math::Mat4 local = math::inverse(scene.world_matrix(parent)) * math::Mat4::trs(position, rotation, {1, 1, 1});
    math::Vec3 lp, ls;
    math::Quat lq;
    math::decompose(local, lp, lq, ls);
    t->position = lp;
    t->rotation = math::normalize(lq);
    t->scale = scale;
}

void sort_events(std::vector<ContactEvent>& events) {
    for (auto& ev : events)
        if (ev.b.bits() < ev.a.bits()) {
            std::swap(ev.a, ev.b);
            ev.normal = ev.normal * -1.0f;
        }
    std::stable_sort(events.begin(), events.end(), [](const ContactEvent& x, const ContactEvent& y) {
        if (x.kind != y.kind) return x.kind > y.kind; // End before Begin: re-entry stays consistent
        if (x.a.bits() != y.a.bits()) return x.a.bits() < y.a.bits();
        return x.b.bits() < y.b.bits();
    });
}

} // namespace detail

struct PhysicsWorld::Impl {
    PhysicsSettings settings;
    std::unique_ptr<detail::World3D> w3;
    std::unique_ptr<detail::World2D> w2;
    std::vector<ContactEvent> events;
    std::vector<ContactEvent> scratch;
};

PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PhysicsWorld::~PhysicsWorld() = default;

Result<std::unique_ptr<PhysicsWorld>> PhysicsWorld::create(const PhysicsSettings& settings) {
    if (settings.max_bodies == 0 || settings.max_bodies > (1u << 23))
        return Error{ErrorCode::InvalidArgument, "physics: max_bodies must be in [1, 2^23]"};
    register_components();
    auto impl = std::make_unique<Impl>();
    impl->settings = settings;
    impl->w3 = detail::World3D::create(settings);
    impl->w2 = detail::World2D::create(settings);
    return std::unique_ptr<PhysicsWorld>(new PhysicsWorld(std::move(impl)));
}

void PhysicsWorld::sync(scene::Scene& scene, float dt) {
    impl_->w3->sync(scene, dt);
    impl_->w2->sync(scene, dt);
}

void PhysicsWorld::step(scene::Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("physics.step");
    impl_->scratch.clear();
    impl_->w3->step(scene, dt, impl_->scratch);
    const std::size_t n3 = impl_->scratch.size();
    impl_->w2->step(scene, dt, impl_->scratch);
    for (std::size_t i = n3; i < impl_->scratch.size(); ++i) impl_->scratch[i].is_2d = true;
    detail::sort_events(impl_->scratch);
    impl_->events.insert(impl_->events.end(), impl_->scratch.begin(), impl_->scratch.end());
}

void PhysicsWorld::clear() {
    impl_->w3->clear();
    impl_->w2->clear();
    impl_->events.clear();
}

void PhysicsWorld::begin_frame() { impl_->events.clear(); }
const std::vector<ContactEvent>& PhysicsWorld::events() const noexcept { return impl_->events; }

std::optional<RaycastHit> PhysicsWorld::raycast(math::Vec3 o, math::Vec3 d, float max, std::uint32_t mask,
                                                Entity ignore) const {
    return impl_->w3->raycast(o, d, max, mask, ignore);
}
std::vector<Entity> PhysicsWorld::overlap_sphere(math::Vec3 c, float r, std::uint32_t mask) const {
    return impl_->w3->overlap_sphere(c, r, mask);
}
std::vector<Entity> PhysicsWorld::overlap_box(math::Vec3 c, math::Vec3 h, math::Quat q, std::uint32_t mask) const {
    return impl_->w3->overlap_box(c, h, q, mask);
}
std::optional<RaycastHit> PhysicsWorld::raycast_2d(math::Vec2 o, math::Vec2 d, float max, std::uint32_t mask,
                                                   Entity ignore) const {
    return impl_->w2->raycast(o, d, max, mask, ignore);
}
std::vector<Entity> PhysicsWorld::overlap_circle_2d(math::Vec2 c, float r, std::uint32_t mask) const {
    return impl_->w2->overlap_circle(c, r, mask);
}
std::vector<Entity> PhysicsWorld::overlap_box_2d(math::Vec2 c, math::Vec2 h, float angle, std::uint32_t mask) const {
    return impl_->w2->overlap_box(c, h, angle, mask);
}

bool PhysicsWorld::has_body(Entity e) const { return impl_->w3->has_body(e) || impl_->w2->has_body(e); }

#define VICTUS_PHYS_DISPATCH(call) (impl_->w2->has_body(e) ? impl_->w2->call : impl_->w3->call)

math::Vec3 PhysicsWorld::linear_velocity(Entity e) const { return VICTUS_PHYS_DISPATCH(linear_velocity(e)); }
void PhysicsWorld::set_linear_velocity(Entity e, math::Vec3 v) { VICTUS_PHYS_DISPATCH(set_linear_velocity(e, v)); }
math::Vec3 PhysicsWorld::angular_velocity(Entity e) const { return VICTUS_PHYS_DISPATCH(angular_velocity(e)); }
void PhysicsWorld::set_angular_velocity(Entity e, math::Vec3 v) { VICTUS_PHYS_DISPATCH(set_angular_velocity(e, v)); }
void PhysicsWorld::add_force(Entity e, math::Vec3 f) { VICTUS_PHYS_DISPATCH(add_force(e, f)); }
void PhysicsWorld::add_impulse(Entity e, math::Vec3 i) { VICTUS_PHYS_DISPATCH(add_impulse(e, i)); }
void PhysicsWorld::add_torque(Entity e, math::Vec3 t) { VICTUS_PHYS_DISPATCH(add_torque(e, t)); }
void PhysicsWorld::add_angular_impulse(Entity e, math::Vec3 i) { VICTUS_PHYS_DISPATCH(add_angular_impulse(e, i)); }

#undef VICTUS_PHYS_DISPATCH

void PhysicsWorld::move_character(Entity e, math::Vec3 v) {
    if (impl_->w2->has_character(e)) impl_->w2->move_character(e, v);
    else impl_->w3->move_character(e, v);
}
bool PhysicsWorld::jump_character(Entity e, float speed) {
    return impl_->w2->has_character(e) ? impl_->w2->jump_character(e, speed) : impl_->w3->jump_character(e, speed);
}
CharacterState PhysicsWorld::character_state(Entity e) const {
    return impl_->w2->has_character(e) ? impl_->w2->character_state(e) : impl_->w3->character_state(e);
}
bool PhysicsWorld::drop_through(Entity e, float seconds) { return impl_->w2->drop_through(e, seconds); }
JointState PhysicsWorld::joint_state(Entity e) const {
    return impl_->w2->has_joint(e) ? impl_->w2->joint_state(e) : impl_->w3->joint_state(e);
}
void PhysicsWorld::set_joint_motor(scene::Scene& scene, Entity e, bool enabled, float speed) {
    if (auto* j = scene.world().get<JointComponent>(e)) {
        j->motor = enabled;
        j->motor_speed = speed;
    }
    if (auto* j = scene.world().get<Joint2DComponent>(e)) {
        j->motor = enabled;
        j->motor_speed = speed;
    }
    impl_->w3->set_joint_motor(e, enabled, speed);
    impl_->w2->set_joint_motor(e, enabled, speed);
}

void PhysicsWorld::set_gravity(math::Vec3 g) { impl_->w3->set_gravity(g); }
void PhysicsWorld::set_gravity_2d(math::Vec2 g) { impl_->w2->set_gravity(g); }
math::Vec3 PhysicsWorld::gravity() const { return impl_->w3->gravity(); }
math::Vec2 PhysicsWorld::gravity_2d() const { return impl_->w2->gravity(); }

PhysicsStats PhysicsWorld::stats() const {
    PhysicsStats s;
    impl_->w3->stats(s);
    impl_->w2->stats(s);
    return s;
}

// ---- systems and binding --------------------------------------------------------------------------

namespace {
struct Binding {
    PhysicsWorld* world{nullptr};
};
} // namespace

void bind(scene::Scene& scene, PhysicsWorld* world) {
    if (auto* b = scene.world().resource<Binding>()) b->world = world;
    else scene.world().set_resource<Binding>().world = world;
}

PhysicsWorld* world_of(scene::Scene& scene) {
    const auto* b = scene.world().resource<Binding>();
    return b ? b->world : nullptr;
}

void add_physics_systems(gameplay::Simulation& sim, PhysicsWorld& world) {
    PhysicsWorld* w = &world;
    sim.add_system("physics.begin_frame", gameplay::Phase::PreUpdate, [w](scene::Scene& scene, const gameplay::FrameTime&) {
        if (world_of(scene) != w) bind(scene, w);
        w->begin_frame();
    });
    sim.add_system("physics.step", gameplay::Phase::FixedUpdate, [w](scene::Scene& scene, const gameplay::FrameTime& t) {
        if (world_of(scene) != w) bind(scene, w);
        w->step(scene, static_cast<float>(t.dt));
    });
}

// ---- collider wireframes ----------------------------------------------------------------------------

namespace {

constexpr math::Color kStatic{0.35f, 0.8f, 0.45f, 1.0f};
constexpr math::Color kDynamic{0.3f, 0.65f, 1.0f, 1.0f};
constexpr math::Color kKinematic{1.0f, 0.75f, 0.25f, 1.0f};
constexpr math::Color kTrigger{0.95f, 0.35f, 0.85f, 1.0f};
constexpr math::Color kCharacter{0.3f, 0.95f, 0.95f, 1.0f};
constexpr math::Color kJoint{1.0f, 0.95f, 0.3f, 1.0f};

math::Color color_for(bool trigger, const BodyType* type) {
    if (trigger) return kTrigger;
    if (!type) return kStatic;
    switch (*type) {
    case BodyType::Dynamic: return kDynamic;
    case BodyType::Kinematic: return kKinematic;
    default: return kStatic;
    }
}

struct Emitter {
    std::vector<DebugLine>& out;
    math::Mat4 m;
    math::Color color;
    math::Vec3 xf(math::Vec3 p) const {
        const math::Vec4 r = m * math::Vec4{p.x, p.y, p.z, 1.0f};
        return {r.x, r.y, r.z};
    }
    void line(math::Vec3 a, math::Vec3 b) { out.push_back({xf(a), xf(b), color}); }
    void circle(math::Vec3 c, math::Vec3 u, math::Vec3 v, float r, int seg = 24, float a0 = 0,
                float a1 = 2.0f * std::numbers::pi_v<float>) {
        math::Vec3 prev = c + u * (r * std::cos(a0)) + v * (r * std::sin(a0));
        for (int i = 1; i <= seg; ++i) {
            const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(seg);
            const math::Vec3 p = c + u * (r * std::cos(a)) + v * (r * std::sin(a));
            line(prev, p);
            prev = p;
        }
    }
    void capsule(math::Vec3 o, float radius, float height) {
        constexpr float pi = std::numbers::pi_v<float>;
        const math::Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
        const float half = std::max(0.0f, height * 0.5f - radius);
        const math::Vec3 top = o + Y * half, bottom = o - Y * half;
        circle(top, X, Z, radius);
        circle(bottom, X, Z, radius);
        circle(top, X, Y, radius, 12, 0, pi);
        circle(top, Z, Y, radius, 12, 0, pi);
        circle(bottom, X, Y, radius, 12, pi, 2 * pi);
        circle(bottom, Z, Y, radius, 12, pi, 2 * pi);
        for (math::Vec3 d : {X, X * -1.0f, Z, Z * -1.0f}) line(top + d * radius, bottom + d * radius);
    }
    void box(math::Vec3 c, math::Vec3 h) {
        math::Vec3 p[8];
        for (int i = 0; i < 8; ++i)
            p[i] = c + math::Vec3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
        static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                             {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : edges) line(p[e[0]], p[e[1]]);
    }
};

bool nearly_equal_points(math::Vec3 a, math::Vec3 b) { return math::length(a - b) < 1e-4f; }

} // namespace

void collider_lines(scene::Scene& scene, std::vector<DebugLine>& out, Entity only) {
    constexpr float pi = std::numbers::pi_v<float>;
    const math::Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    scene.world().each<CharacterControllerComponent>([&](Entity e, CharacterControllerComponent& c) {
        if (!only.is_null() && e != only) return;
        // Characters stay upright: draw at the world position without the entity's rotation/scale.
        const math::Vec3 feet = scene.world_matrix(e).transform_point({});
        Emitter em{out, math::Mat4::trs(feet, {}, {1, 1, 1}), kCharacter};
        em.capsule({0, c.height * 0.5f, 0}, c.radius, std::max(c.height, 2 * c.radius));
        em.line({0, 0, 0}, {0, 0, c.radius * 1.5f}); // forward hint
    });
    scene.world().each<JointComponent>([&](Entity e, JointComponent& j) {
        if (!only.is_null() && e != only) return;
        const math::Mat4 m = scene.world_matrix(e);
        const math::Vec3 a = m.transform_point(j.anchor);
        Emitter em{out, math::Mat4::trs(a, {}, {1, 1, 1}), kJoint};
        constexpr float s = 0.12f;
        em.line(X * -s, X * s);
        em.line(Y * -s, Y * s);
        em.line(Z * -s, Z * s);
        if (j.type == JointComponent::Type::Hinge || j.type == JointComponent::Type::Slider) {
            math::Vec3 axis = m.transform_point(j.axis) - m.transform_point({});
            if (math::length(axis) > 1e-6f) em.line(axis * -0.4f / math::length(axis), axis * 0.4f / math::length(axis));
        }
        // Line to the connected body (or the world point for distance joints).
        math::Vec3 to = a;
        if (!j.connected.is_nil()) {
            const Entity o = scene.find(j.connected);
            if (!o.is_null() && scene.world().alive(o))
                to = scene.world_matrix(o).transform_point(j.type == JointComponent::Type::Distance ? j.connected_anchor
                                                                                                   : math::Vec3{});
        } else if (j.type == JointComponent::Type::Distance) {
            to = j.connected_anchor;
        }
        if (!nearly_equal_points(a, to)) out.push_back({a, to, kJoint});
    });
    scene.world().each<ColliderComponent>([&](Entity e, ColliderComponent& c) {
        if (!only.is_null() && e != only) return;
        if (scene.world().get<CharacterControllerComponent>(e)) return;
        const auto* rb = scene.world().get<RigidBodyComponent>(e);
        Emitter em{out, scene.world_matrix(e), color_for(c.is_trigger, rb ? &rb->type : nullptr)};
        const math::Vec3 o = c.offset;
        switch (c.shape) {
        case ColliderComponent::Shape::Box: em.box(o, c.size * 0.5f); break;
        case ColliderComponent::Shape::Sphere:
            em.circle(o, X, Y, c.radius);
            em.circle(o, X, Z, c.radius);
            em.circle(o, Y, Z, c.radius);
            break;
        case ColliderComponent::Shape::Capsule: em.capsule(o, c.radius, c.height); break;
        case ColliderComponent::Shape::Cylinder: {
            const float half = c.height * 0.5f;
            const math::Vec3 top = o + Y * half, bottom = o - Y * half;
            em.circle(top, X, Z, c.radius);
            em.circle(bottom, X, Z, c.radius);
            for (math::Vec3 d : {X, X * -1.0f, Z, Z * -1.0f}) em.line(top + d * c.radius, bottom + d * c.radius);
            break;
        }
        }
    });
    scene.world().each<CharacterController2DComponent>([&](Entity e, CharacterController2DComponent& c) {
        if (!only.is_null() && e != only) return;
        // 2D characters stay upright and are centred on the entity origin.
        const math::Vec3 centre = scene.world_matrix(e).transform_point({});
        Emitter em{out, math::Mat4::trs(centre, {}, {1, 1, 1}), kCharacter};
        const float r = std::max(0.01f, c.size.x * 0.5f);
        const float half = std::max(0.0f, c.size.y * 0.5f - r);
        em.circle(Y * half, X, Y, r, 12, 0, pi);
        em.circle(Y * -half, X, Y, r, 12, pi, 2 * pi);
        em.line(Y * half + X * r, Y * -half + X * r);
        em.line(Y * half - X * r, Y * -half - X * r);
    });
    scene.world().each<Joint2DComponent>([&](Entity e, Joint2DComponent& j) {
        if (!only.is_null() && e != only) return;
        const math::Mat4 m = scene.world_matrix(e);
        const math::Vec3 a = m.transform_point({j.anchor.x, j.anchor.y, 0});
        Emitter em{out, math::Mat4::trs(a, {}, {1, 1, 1}), kJoint};
        constexpr float s = 0.12f;
        if (j.type == Joint2DComponent::Type::Revolute || j.type == Joint2DComponent::Type::Wheel) em.circle({}, X, Y, s, 12);
        em.line(X * -s, X * s);
        em.line(Y * -s, Y * s);
        if (j.type == Joint2DComponent::Type::Prismatic || j.type == Joint2DComponent::Type::Wheel) {
            const math::Vec3 axis = m.transform_point({j.axis.x, j.axis.y, 0}) - m.transform_point({});
            if (math::length(axis) > 1e-6f) em.line(axis * -0.4f / math::length(axis), axis * 0.4f / math::length(axis));
        }
        const bool distance = j.type == Joint2DComponent::Type::Distance;
        const math::Vec3 local{j.connected_anchor.x, j.connected_anchor.y, 0};
        math::Vec3 to = a;
        if (!j.connected.is_nil()) {
            const Entity o = scene.find(j.connected);
            if (!o.is_null() && scene.world().alive(o)) to = scene.world_matrix(o).transform_point(distance ? local : math::Vec3{});
        } else if (distance) {
            to = local;
        }
        if (!nearly_equal_points(a, to)) out.push_back({a, to, kJoint});
    });
    scene.world().each<Collider2DComponent>([&](Entity e, Collider2DComponent& c) {
        if (!only.is_null() && e != only) return;
        if (scene.world().get<CharacterController2DComponent>(e)) return;
        if (c.one_way) {
            // An arrow along the pass-through direction (local +Y).
            Emitter arrow{out, scene.world_matrix(e), kKinematic};
            const math::Vec3 o{c.offset.x, c.offset.y, 0};
            const float len = std::max(0.2f, std::min(0.5f, c.size.y));
            arrow.line(o, o + Y * len);
            arrow.line(o + Y * len, o + Y * (len * 0.6f) + X * (len * 0.3f));
            arrow.line(o + Y * len, o + Y * (len * 0.6f) - X * (len * 0.3f));
        }
        const auto* rb = scene.world().get<RigidBody2DComponent>(e);
        Emitter em{out, scene.world_matrix(e), color_for(c.is_trigger, rb ? &rb->type : nullptr)};
        const math::Vec3 o{c.offset.x, c.offset.y, 0};
        switch (c.shape) {
        case Collider2DComponent::Shape::Box: {
            const math::Vec3 h{c.size.x * 0.5f, c.size.y * 0.5f, 0};
            const math::Vec3 p[4] = {o + math::Vec3{-h.x, -h.y, 0}, o + math::Vec3{h.x, -h.y, 0},
                                     o + math::Vec3{h.x, h.y, 0}, o + math::Vec3{-h.x, h.y, 0}};
            for (int i = 0; i < 4; ++i) em.line(p[i], p[(i + 1) % 4]);
            break;
        }
        case Collider2DComponent::Shape::Circle:
            em.circle(o, X, Y, c.radius);
            em.line(o, o + X * c.radius); // shows rotation
            break;
        case Collider2DComponent::Shape::Capsule: {
            const float r = c.size.x * 0.5f;
            const float half = std::max(0.0f, c.size.y * 0.5f - r);
            const math::Vec3 top = o + Y * half, bottom = o - Y * half;
            em.circle(top, X, Y, r, 12, 0, pi);
            em.circle(bottom, X, Y, r, 12, pi, 2 * pi);
            em.line(top + X * r, bottom + X * r);
            em.line(top - X * r, bottom - X * r);
            break;
        }
        }
    });
}

} // namespace victus::physics
