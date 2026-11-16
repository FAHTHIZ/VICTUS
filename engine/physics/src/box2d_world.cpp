// SPDX-License-Identifier: Apache-2.0
// 2D physics backend on Box2D 3.2.0 (MIT). Box2D types never leave this file.
#include "internal.hpp"

#include "victus/core/jobs.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"

#include <box2d/box2d.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace victus::physics::detail {

namespace {

b2Vec2 v2(math::Vec2 v) { return {v.x, v.y}; }

struct TaskPool {
    jobs::JobSystem* jobs{nullptr};
    std::deque<jobs::Counter> counters; // stable addresses; reset after every step
};

void* enqueue_task(b2TaskCallbackFcn* task, void* task_context, void* user_context) {
    auto* pool = static_cast<TaskPool*>(user_context);
    jobs::Counter& c = pool->counters.emplace_back();
    pool->jobs->submit([task, task_context] { task(task_context); }, &c, jobs::Priority::High);
    return &c;
}

void finish_task(void* user_task, void* user_context) {
    auto* pool = static_cast<TaskPool*>(user_context);
    pool->jobs->wait(*static_cast<jobs::Counter*>(user_task));
}

constexpr std::uint64_t kQueryCategory = ~0ull;

class Box2DWorld final : public World2D {
public:
    explicit Box2DWorld(const PhysicsSettings& s) : settings_(s) {
        std::copy(s.layer_matrix, s.layer_matrix + kMaxLayers, matrix_);
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity = v2(s.gravity_2d);
        if (s.jobs) {
            pool_.jobs = s.jobs;
            def.workerCount = static_cast<int>(s.jobs->worker_count()) + 1;
            def.enqueueTask = enqueue_task;
            def.finishTask = finish_task;
            def.userTaskContext = &pool_;
        }
        world_ = b2CreateWorld(&def);
    }
    ~Box2DWorld() override { b2DestroyWorld(world_); }

    void sync(scene::Scene& scene, float dt) override {
        VICTUS_PROFILE_SCOPE("physics.sync2d");
        ++generation_;
        scene.world().each<Collider2DComponent>([&](Entity e, Collider2DComponent& col) {
            const RigidBody2DComponent* rb = scene.world().get<RigidBody2DComponent>(e);
            const Pose pose = world_pose(scene, e);
            const math::Vec3 scale = abs(pose.scale);
            const math::Vec2 p{pose.position.x, pose.position.y};
            const float angle = z_angle(pose.rotation);
            auto it = bodies_.find(e.bits());
            if (it != bodies_.end()) {
                Entry& b = it->second;
                const bool same = b.collider == col && b.has_rb == (rb != nullptr) && (!rb || b.rb == *rb) &&
                                  std::abs(b.scale.x - scale.x) < 1e-4f && std::abs(b.scale.y - scale.y) < 1e-4f;
                if (!same) {
                    destroy(b);
                    bodies_.erase(it);
                    it = bodies_.end();
                }
            }
            if (it == bodies_.end()) {
                Entry b;
                create(e, col, rb, p, angle, {scale.x, scale.y}, b);
                it = bodies_.emplace(e.bits(), b).first;
                shapes_[b2StoreShapeId(b.shape)] = e;
            }
            Entry& b = it->second;
            b.seen = generation_;
            b.z = pose.position.z;
            b.rotation3d = pose.rotation;
            const bool moved = std::abs(p.x - b.position.x) > 1e-5f || std::abs(p.y - b.position.y) > 1e-5f ||
                               std::abs(angle - b.angle) > 1e-5f;
            if (b.type == b2_kinematicBody) {
                if (moved || b.moving)
                    b2Body_SetTargetTransform(b.body, b2WorldTransform{v2(p), b2MakeRot(angle)}, std::max(dt, 1e-4f),
                                              true);
                b.moving = moved;
            } else if (moved) {
                b2Body_SetTransform(b.body, v2(p), b2MakeRot(angle));
                if (b.type == b2_dynamicBody) b2Body_SetAwake(b.body, true);
            }
            b.position = p;
            b.angle = angle;
        });
        for (auto it = bodies_.begin(); it != bodies_.end();) {
            if (it->second.seen != generation_) {
                destroy(it->second);
                it = bodies_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void step(scene::Scene& scene, float dt, std::vector<ContactEvent>& events) override {
        sync(scene, dt);
        {
            VICTUS_PROFILE_SCOPE("physics.step2d");
            const auto t0 = std::chrono::steady_clock::now();
            if (!bodies_.empty()) b2World_Step(world_, dt, std::max(1, settings_.substeps_2d));
            pool_.counters.clear();
            last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++steps_;
        }
        VICTUS_PROFILE_SCOPE("physics.writeback2d");
        for (auto& [key, b] : bodies_) {
            if (b.type != b2_dynamicBody || !b2Body_IsAwake(b.body)) continue;
            const Entity e{static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32)};
            if (!scene.world().alive(e)) continue;
            const b2WorldTransform xf = b2Body_GetTransform(b.body);
            b.position = {static_cast<float>(xf.p.x), static_cast<float>(xf.p.y)};
            b.angle = b2Rot_GetAngle(xf.q);
            b.rotation3d = with_z_angle(b.rotation3d, b.angle);
            write_world_pose(scene, e, {b.position.x, b.position.y, b.z}, b.rotation3d);
        }
        const std::size_t before = events.size();
        collect_events(events);
        contacts_last_ = static_cast<std::uint32_t>(events.size() - before);
    }

    void clear() override {
        for (auto& [k, b] : bodies_) destroy(b);
        bodies_.clear();
        shapes_.clear();
        // Flush end events produced by the destruction so the next session starts clean.
        b2World_Step(world_, 0.0f, 1);
    }

    bool has_body(Entity e) const override { return bodies_.contains(e.bits()); }

    std::optional<RaycastHit> raycast(math::Vec2 o, math::Vec2 d, float max, std::uint32_t mask,
                                      Entity ignore) const override {
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len <= 1e-8f || max <= 0) return std::nullopt;
        const b2Vec2 translation{d.x / len * max, d.y / len * max};
        struct Ctx {
            const Box2DWorld* self;
            Entity ignore;
            Entity best;
            float fraction{2.0f};
            b2Vec2 point{}, normal{};
        } ctx{this, ignore, {}};
        b2QueryFilter filter{kQueryCategory, mask};
        b2World_CastRay(
            world_, v2(o), translation, filter,
            [](b2ShapeId shape, b2Pos point, b2Vec2 normal, float fraction, void* user) -> float {
                auto& c = *static_cast<Ctx*>(user);
                if (b2Shape_IsSensor(shape)) return -1.0f;
                const Entity e = c.self->entity_of(shape);
                if (e.is_null() || e == c.ignore) return -1.0f;
                if (fraction < c.fraction) {
                    c.fraction = fraction;
                    c.best = e;
                    c.point = {static_cast<float>(point.x), static_cast<float>(point.y)};
                    c.normal = normal;
                }
                return fraction; // clip to the closest hit so far
            },
            &ctx);
        if (ctx.best.is_null()) return std::nullopt;
        RaycastHit hit;
        hit.entity = ctx.best;
        hit.point = {ctx.point.x, ctx.point.y, 0};
        hit.normal = {ctx.normal.x, ctx.normal.y, 0};
        hit.distance = ctx.fraction * max;
        return hit;
    }

    std::vector<Entity> overlap_circle(math::Vec2 c, float r, std::uint32_t mask) const override {
        if (r <= 0) return {};
        const b2Vec2 origin{0, 0};
        const b2ShapeProxy proxy = b2MakeProxy(&origin, 1, r);
        return overlap(v2(c), proxy, mask);
    }

    std::vector<Entity> overlap_box(math::Vec2 c, math::Vec2 h, float angle, std::uint32_t mask) const override {
        h = {std::abs(h.x), std::abs(h.y)};
        if (h.x <= 0 || h.y <= 0) return {};
        const b2Polygon box = b2MakeOffsetBox(h.x, h.y, {0, 0}, b2MakeRot(angle));
        const b2ShapeProxy proxy = b2MakeProxy(box.vertices, box.count, 0.0f);
        return overlap(v2(c), proxy, mask);
    }

    math::Vec3 linear_velocity(Entity e) const override {
        const Entry* b = find(e);
        if (!b) return {};
        const b2Vec2 v = b2Body_GetLinearVelocity(b->body);
        return {v.x, v.y, 0};
    }
    void set_linear_velocity(Entity e, math::Vec3 v) override {
        if (const Entry* b = dynamic(e)) b2Body_SetLinearVelocity(b->body, {v.x, v.y});
    }
    math::Vec3 angular_velocity(Entity e) const override {
        const Entry* b = find(e);
        return b ? math::Vec3{0, 0, b2Body_GetAngularVelocity(b->body)} : math::Vec3{};
    }
    void set_angular_velocity(Entity e, math::Vec3 v) override {
        if (const Entry* b = dynamic(e)) b2Body_SetAngularVelocity(b->body, v.z);
    }
    void add_force(Entity e, math::Vec3 f) override {
        if (const Entry* b = dynamic(e)) b2Body_ApplyForceToCenter(b->body, {f.x, f.y}, true);
    }
    void add_impulse(Entity e, math::Vec3 i) override {
        if (const Entry* b = dynamic(e)) b2Body_ApplyLinearImpulseToCenter(b->body, {i.x, i.y}, true);
    }
    void add_torque(Entity e, math::Vec3 t) override {
        if (const Entry* b = dynamic(e)) b2Body_ApplyTorque(b->body, t.z, true);
    }
    void add_angular_impulse(Entity e, math::Vec3 i) override {
        if (const Entry* b = dynamic(e)) b2Body_ApplyAngularImpulse(b->body, i.z, true);
    }
    void set_gravity(math::Vec2 g) override { b2World_SetGravity(world_, v2(g)); }
    math::Vec2 gravity() const override {
        const b2Vec2 g = b2World_GetGravity(world_);
        return {g.x, g.y};
    }
    void stats(PhysicsStats& out) const override {
        out.bodies_2d = static_cast<std::uint32_t>(bodies_.size());
        out.awake_2d = static_cast<std::uint32_t>(b2World_GetAwakeBodyCount(world_));
        out.contacts_last_step += contacts_last_;
        out.last_step_ms += last_ms_;
    }

private:
    struct Entry {
        b2BodyId body{};
        b2ShapeId shape{};
        b2BodyType type{b2_staticBody};
        Collider2DComponent collider;
        RigidBody2DComponent rb;
        bool has_rb{false};
        math::Vec2 scale{1, 1};
        math::Vec2 position;
        float angle{0};
        float z{0};
        math::Quat rotation3d;
        std::uint64_t seen{0};
        bool moving{false};
    };

    const Entry* find(Entity e) const {
        const auto it = bodies_.find(e.bits());
        return it == bodies_.end() ? nullptr : &it->second;
    }
    const Entry* dynamic(Entity e) const {
        const Entry* b = find(e);
        return b && b->type == b2_dynamicBody ? b : nullptr;
    }
    Entity entity_of(b2ShapeId s) const {
        const auto it = shapes_.find(b2StoreShapeId(s));
        return it == shapes_.end() ? Entity{} : it->second;
    }

    void create(Entity e, const Collider2DComponent& col, const RigidBody2DComponent* rb, math::Vec2 p, float angle,
                math::Vec2 scale, Entry& out) {
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        if (rb && rb->type == BodyType::Dynamic) bd.type = b2_dynamicBody;
        else if (rb && rb->type == BodyType::Kinematic) bd.type = b2_kinematicBody;
        // Static triggers become kinematic so they report static/kinematic visitors too.
        if (col.is_trigger && bd.type == b2_staticBody) bd.type = b2_kinematicBody;
        bd.position = v2(p);
        bd.rotation = b2MakeRot(angle);
        if (rb) {
            bd.linearDamping = std::max(0.0f, rb->linear_damping);
            bd.angularDamping = std::max(0.0f, rb->angular_damping);
            bd.gravityScale = rb->gravity_scale;
            bd.motionLocks.angularZ = rb->fixed_rotation;
            bd.isBullet = rb->bullet;
            bd.isAwake = rb->start_awake;
            if (bd.type == b2_dynamicBody) {
                bd.linearVelocity = v2(rb->linear_velocity);
                bd.angularVelocity = rb->angular_velocity;
            }
        }
        out.body = b2CreateBody(world_, &bd);
        out.type = bd.type;

        b2ShapeDef sd = b2DefaultShapeDef();
        sd.material.friction = std::max(0.0f, col.friction);
        sd.material.restitution = std::clamp(col.restitution, 0.0f, 1.0f);
        sd.isSensor = col.is_trigger;
        sd.enableSensorEvents = true;
        sd.enableContactEvents = true;
        const std::uint32_t layer = std::min<std::uint32_t>(col.layer, 15u);
        sd.filter.categoryBits = 1ull << layer;
        sd.filter.maskBits = static_cast<std::uint64_t>(col.mask & matrix_[layer] & 0xFFFFu);
        const b2Vec2 off{col.offset.x * scale.x, col.offset.y * scale.y};
        switch (col.shape) {
        case Collider2DComponent::Shape::Box: {
            const float hx = std::max(1e-3f, col.size.x * scale.x * 0.5f);
            const float hy = std::max(1e-3f, col.size.y * scale.y * 0.5f);
            const float r = std::clamp(col.rounding, 0.0f, std::min(hx, hy) * 0.99f);
            const b2Polygon poly = b2MakeOffsetRoundedBox(hx - r, hy - r, off, b2MakeRot(0), r);
            out.shape = b2CreatePolygonShape(out.body, &sd, &poly);
            break;
        }
        case Collider2DComponent::Shape::Circle: {
            const b2Circle circle{off, std::max(1e-3f, col.radius * std::max(scale.x, scale.y))};
            out.shape = b2CreateCircleShape(out.body, &sd, &circle);
            break;
        }
        case Collider2DComponent::Shape::Capsule: {
            const float r = std::max(1e-3f, col.size.x * scale.x * 0.5f);
            const float half = std::max(0.0f, col.size.y * scale.y * 0.5f - r);
            if (half <= 1e-4f) {
                const b2Circle circle{off, r};
                out.shape = b2CreateCircleShape(out.body, &sd, &circle);
            } else {
                const b2Capsule cap{{off.x, off.y - half}, {off.x, off.y + half}, r};
                out.shape = b2CreateCapsuleShape(out.body, &sd, &cap);
            }
            break;
        }
        }
        if (bd.type == b2_dynamicBody && rb) {
            b2MassData md = b2Body_GetMassData(out.body);
            if (md.mass > 0) {
                const float k = std::max(1e-4f, rb->mass) / md.mass;
                md.mass *= k;
                md.rotationalInertia *= k;
                b2Body_SetMassData(out.body, md);
            }
        }
        out.collider = col;
        out.has_rb = rb != nullptr;
        if (rb) out.rb = *rb;
        out.scale = scale;
        out.position = p;
        out.angle = angle;
        (void)e;
    }

    void destroy(Entry& b) {
        if (!b2Body_IsValid(b.body)) return;
        // Keep the shape -> entity mapping until the next event pass so end events still resolve.
        retired_.push_back(b2StoreShapeId(b.shape));
        b2DestroyBody(b.body);
        b.body = {};
    }

    void collect_events(std::vector<ContactEvent>& events) {
        const b2ContactEvents ce = b2World_GetContactEvents(world_);
        for (int i = 0; i < ce.beginCount; ++i) {
            const auto& ev = ce.beginEvents[i];
            ContactEvent out;
            out.kind = ContactEvent::Kind::Begin;
            out.a = entity_of(ev.shapeIdA);
            out.b = entity_of(ev.shapeIdB);
            if (out.a.is_null() || out.b.is_null()) continue;
            if (b2Contact_IsValid(ev.contactId)) {
                const b2ContactData data = b2Contact_GetData(ev.contactId);
                out.normal = {data.manifold.normal.x, data.manifold.normal.y, 0};
                if (data.manifold.pointCount > 0) {
                    const b2Pos c = b2Body_GetWorldCenter(b2Shape_GetBody(ev.shapeIdA));
                    const b2Vec2 a = data.manifold.points[0].anchorA;
                    out.point = {static_cast<float>(c.x) + a.x, static_cast<float>(c.y) + a.y, 0};
                }
            }
            events.push_back(out);
        }
        for (int i = 0; i < ce.endCount; ++i) {
            const auto& ev = ce.endEvents[i];
            ContactEvent out;
            out.kind = ContactEvent::Kind::End;
            out.a = entity_of(ev.shapeIdA);
            out.b = entity_of(ev.shapeIdB);
            if (!out.a.is_null() && !out.b.is_null()) events.push_back(out);
        }
        const b2SensorEvents se = b2World_GetSensorEvents(world_);
        for (int i = 0; i < se.beginCount; ++i) {
            ContactEvent out;
            out.kind = ContactEvent::Kind::Begin;
            out.trigger = true;
            out.a = entity_of(se.beginEvents[i].sensorShapeId);
            out.b = entity_of(se.beginEvents[i].visitorShapeId);
            if (!out.a.is_null() && !out.b.is_null() && out.a != out.b) events.push_back(out);
        }
        for (int i = 0; i < se.endCount; ++i) {
            ContactEvent out;
            out.kind = ContactEvent::Kind::End;
            out.trigger = true;
            out.a = entity_of(se.endEvents[i].sensorShapeId);
            out.b = entity_of(se.endEvents[i].visitorShapeId);
            if (!out.a.is_null() && !out.b.is_null() && out.a != out.b) events.push_back(out);
        }
        for (std::uint64_t s : retired_) shapes_.erase(s);
        retired_.clear();
    }

    std::vector<Entity> overlap(b2Vec2 origin, const b2ShapeProxy& proxy, std::uint32_t mask) const {
        struct Ctx {
            const Box2DWorld* self;
            std::vector<Entity> out;
        } ctx{this, {}};
        b2World_OverlapShape(
            world_, origin, &proxy, b2QueryFilter{kQueryCategory, mask},
            [](b2ShapeId shape, void* user) -> bool {
                auto& c = *static_cast<Ctx*>(user);
                if (b2Shape_IsSensor(shape)) return true;
                const Entity e = c.self->entity_of(shape);
                if (!e.is_null()) c.out.push_back(e);
                return true;
            },
            &ctx);
        std::sort(ctx.out.begin(), ctx.out.end(), [](Entity a, Entity b) { return a.bits() < b.bits(); });
        ctx.out.erase(std::unique(ctx.out.begin(), ctx.out.end()), ctx.out.end());
        return std::move(ctx.out);
    }

    PhysicsSettings settings_;
    std::uint32_t matrix_[kMaxLayers]{};
    TaskPool pool_;
    b2WorldId world_{};
    std::unordered_map<std::uint64_t, Entry> bodies_;
    std::unordered_map<std::uint64_t, Entity> shapes_;
    std::vector<std::uint64_t> retired_;
    std::uint64_t generation_{0};
    std::uint64_t steps_{0};
    std::uint32_t contacts_last_{0};
    double last_ms_{0};
};

} // namespace

std::unique_ptr<World2D> World2D::create(const PhysicsSettings& settings) {
    return std::make_unique<Box2DWorld>(settings);
}

} // namespace victus::physics::detail
