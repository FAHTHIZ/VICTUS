// SPDX-License-Identifier: Apache-2.0
// 2D physics backend on Box2D 3.2.0 (MIT). Box2D types never leave this file.
#include "internal.hpp"

#include "victus/core/jobs.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"

#include <box2d/box2d.h>

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <deque>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace victus::physics::detail {

namespace {

b2Vec2 v2(math::Vec2 v) { return {v.x, v.y}; }
b2Vec2 f2(b2Pos p) { return {static_cast<float>(p.x), static_cast<float>(p.y)}; }
b2Pos pos(b2Vec2 v) { return b2Pos{v.x, v.y}; }
float radians(float deg) { return deg * (std::numbers::pi_v<float> / 180.0f); }
float degrees(float rad) { return rad * (180.0f / std::numbers::pi_v<float>); }

// Shape user data: a small flag word (no allocation, safe to read from Box2D worker threads).
constexpr std::uintptr_t kOneWay = 1;
constexpr std::uintptr_t kCharacter = 2;
std::uintptr_t flags_of(b2ShapeId s) { return reinterpret_cast<std::uintptr_t>(b2Shape_GetUserData(s)); }
b2Vec2 up_of(b2ShapeId s) { return b2RotateVector(b2Body_GetRotation(b2Shape_GetBody(s)), {0, 1}); }

// A one-way platform only pushes along its up axis and only while the other shape is not buried in it
// (so bodies rising from below pass through and land once they are on top).
void one_way_presolve(b2ShapeId a, b2ShapeId b, b2Manifold* m, void*) {
    const bool wa = (flags_of(a) & kOneWay) != 0, wb = (flags_of(b) & kOneWay) != 0;
    if (wa == wb || m->pointCount == 0) return;
    const b2Vec2 n = wa ? m->normal : b2Neg(m->normal); // from the platform towards the other shape
    const b2Vec2 up = up_of(wa ? a : b);
    float deepest = FLT_MAX;
    for (int i = 0; i < m->pointCount; ++i) deepest = std::min(deepest, m->points[i].separation);
    if (b2Dot(n, up) < 0.7f || deepest < -0.08f) m->pointCount = 0;
}
bool one_way_precontinuous(b2ShapeId a, b2ShapeId b, b2Pos, b2Vec2 normal, void*) {
    const bool wa = (flags_of(a) & kOneWay) != 0, wb = (flags_of(b) & kOneWay) != 0;
    if (wa == wb) return true;
    const b2Vec2 n = wa ? normal : b2Neg(normal);
    return b2Dot(n, up_of(wa ? a : b)) > 0.7f;
}

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
        b2World_SetPreSolveCallback(world_, one_way_presolve, one_way_precontinuous, nullptr);
    }
    ~Box2DWorld() override { b2DestroyWorld(world_); }

    void sync(scene::Scene& scene, float dt) override {
        VICTUS_PROFILE_SCOPE("physics.sync2d");
        ++generation_;
        scene.world().each<Collider2DComponent>([&](Entity e, Collider2DComponent& col) {
            if (scene.world().get<CharacterController2DComponent>(e)) return; // the character owns its shape
            const RigidBody2DComponent* rb = scene.world().get<RigidBody2DComponent>(e);
            const Pose pose = world_pose(scene, e);
            // Outline shapes keep the sign so mirrored copies stay mirrored.
            const bool outline = col.shape == Collider2DComponent::Shape::Polygon || col.shape == Collider2DComponent::Shape::Chain;
            const math::Vec3 scale = outline ? pose.scale : abs(pose.scale);
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
                if (auto f = failed_.find(e.bits()); f != failed_.end()) {
                    const Failed& x = f->second;
                    if (x.collider == col && x.has_rb == (rb != nullptr) && (!rb || x.rb == *rb) &&
                        std::abs(x.scale.x - scale.x) < 1e-4f && std::abs(x.scale.y - scale.y) < 1e-4f) {
                        f->second.seen = generation_;
                        return;
                    }
                    failed_.erase(f);
                    warned_.erase(e.bits());
                }
                Entry b;
                create(e, col, rb, p, angle, {scale.x, scale.y}, b);
                if (!b2Body_IsValid(b.body)) {
                    failed_[e.bits()] = Failed{col, rb != nullptr, rb ? *rb : RigidBody2DComponent{}, {scale.x, scale.y}, generation_};
                    return;
                }
                it = bodies_.emplace(e.bits(), b).first;
                for (const b2ShapeId s : b.shapes) shapes_[b2StoreShapeId(s)] = e;
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
        std::erase_if(failed_, [&](const auto& kv) { return kv.second.seen != generation_; });
        sync_compounds(scene);
        sync_characters(scene);
        sync_joints(scene);
    }

    void step(scene::Scene& scene, float dt, std::vector<ContactEvent>& events) override {
        sync(scene, dt);
        update_characters(scene, dt);
        {
            VICTUS_PROFILE_SCOPE("physics.step2d");
            const auto t0 = std::chrono::steady_clock::now();
            if (!bodies_.empty() || !chars_.empty()) b2World_Step(world_, dt, std::max(1, settings_.substeps_2d));
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
        check_joints();
        const std::size_t before = events.size();
        collect_events(events);
        contacts_last_ = static_cast<std::uint32_t>(events.size() - before);
    }

    void clear() override {
        for (auto& [k, j] : joints_) destroy_joint(j);
        joints_.clear();
        for (auto& [k, c] : chars_) destroy_character(c);
        chars_.clear();
        for (auto& [k, b] : bodies_) destroy(b);
        bodies_.clear();
        for (auto& [k, c] : compounds_) destroy_compound(c);
        compounds_.clear();
        shapes_.clear();
        failed_.clear();
        warned_.clear();
        // Flush end events produced by the destruction so the next session starts clean.
        b2World_Step(world_, 0.0f, 1);
    }

    bool has_body(Entity e) const override {
        return bodies_.contains(e.bits()) || chars_.contains(e.bits()) || compounds_.contains(e.bits());
    }

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
        if (const auto c = chars_.find(e.bits()); c != chars_.end()) return {c->second.velocity.x, c->second.velocity.y, 0};
        const Entry* b = find(e);
        if (!b) return {};
        const b2Vec2 v = b2Body_GetLinearVelocity(b->body);
        return {v.x, v.y, 0};
    }
    void set_linear_velocity(Entity e, math::Vec3 v) override {
        if (const auto c = chars_.find(e.bits()); c != chars_.end()) {
            c->second.velocity = {v.x, v.y};
            c->second.grounded = false; // an explicit launch leaves the ground
            return;
        }
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
    bool has_character(Entity e) const override { return chars_.contains(e.bits()); }
    void move_character(Entity e, math::Vec3 v) override {
        if (const auto c = chars_.find(e.bits()); c != chars_.end()) {
            c->second.desired = v.x;
            c->second.desired_y = v.y;
        }
    }
    bool jump_character(Entity e, float speed) override {
        const auto c = chars_.find(e.bits());
        if (c == chars_.end() || !c->second.grounded || speed <= 0) return false;
        c->second.jump = speed;
        return true;
    }
    bool drop_through(Entity e, float seconds) override {
        const auto c = chars_.find(e.bits());
        if (c == chars_.end()) return false;
        c->second.drop = std::max(c->second.drop, std::max(0.0f, seconds));
        c->second.grounded = c->second.grounded && !c->second.on_one_way;
        return true;
    }
    CharacterState character_state(Entity e) const override {
        CharacterState s;
        const auto it = chars_.find(e.bits());
        if (it == chars_.end()) return s;
        const Character& c = it->second;
        s.valid = true;
        s.grounded = c.grounded;
        s.on_steep_slope = c.steep;
        if (c.grounded || c.steep) {
            s.ground_normal = {c.ground_normal.x, c.ground_normal.y, 0};
            s.ground = c.ground;
        }
        s.velocity = {c.velocity.x, c.velocity.y, 0};
        return s;
    }
    bool has_joint(Entity e) const override { return joints_.contains(e.bits()); }
    JointState joint_state(Entity e) const override {
        JointState s;
        const auto it = joints_.find(e.bits());
        if (it == joints_.end()) return s;
        const Joint& j = it->second;
        s.valid = true;
        s.broken = j.broken;
        s.active = !j.broken && b2Joint_IsValid(j.id);
        s.force = j.force;
        if (!s.active) return s;
        switch (j.comp.type) {
        case Joint2DComponent::Type::Revolute: s.angle = degrees(b2RevoluteJoint_GetCurrentAngle(j.id)); break;
        case Joint2DComponent::Type::Prismatic: s.position = b2PrismaticJoint_GetCurrentTranslation(j.id); break;
        case Joint2DComponent::Type::Distance: s.position = b2DistanceJoint_GetCurrentLength(j.id); break;
        case Joint2DComponent::Type::Wheel: {
            const b2Transform fa = b2Joint_GetLocalFrameA(j.id), fb = b2Joint_GetLocalFrameB(j.id);
            const b2Vec2 pa = f2(b2Body_GetWorldPoint(j.a, fa.p)), pb = f2(b2Body_GetWorldPoint(j.b, fb.p));
            const b2Vec2 axis = b2RotateVector(b2MulRot(b2Body_GetRotation(j.a), fa.q), {1, 0});
            s.position = b2Dot(b2Sub(pb, pa), axis);
            break;
        }
        case Joint2DComponent::Type::Weld: break;
        }
        return s;
    }
    void set_joint_motor(Entity e, bool enabled, float speed) override {
        const auto it = joints_.find(e.bits());
        if (it == joints_.end()) return;
        it->second.comp.motor = enabled;
        it->second.comp.motor_speed = speed;
        apply_motor(it->second);
    }
    void set_gravity(math::Vec2 g) override { b2World_SetGravity(world_, v2(g)); }
    math::Vec2 gravity() const override {
        const b2Vec2 g = b2World_GetGravity(world_);
        return {g.x, g.y};
    }
    void stats(PhysicsStats& out) const override {
        out.bodies_2d = static_cast<std::uint32_t>(bodies_.size() + chars_.size() + compounds_.size());
        out.characters_2d = static_cast<std::uint32_t>(chars_.size());
        out.joints_2d = 0;
        for (const auto& [k, j] : joints_) out.joints_2d += (!j.broken && b2Joint_IsValid(j.id)) ? 1u : 0u;
        out.awake_2d = static_cast<std::uint32_t>(b2World_GetAwakeBodyCount(world_));
        out.contacts_last_step += contacts_last_;
        out.last_step_ms += last_ms_;
    }

private:
    struct Entry {
        b2BodyId body{};
        std::vector<b2ShapeId> shapes; // polygons split into convex pieces and chains have several
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
        sd.enablePreSolveEvents = col.one_way && !col.is_trigger;
        if (sd.enablePreSolveEvents) sd.userData = reinterpret_cast<void*>(kOneWay);
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
            out.shapes.push_back(b2CreatePolygonShape(out.body, &sd, &poly));
            break;
        }
        case Collider2DComponent::Shape::Circle: {
            const b2Circle circle{off, std::max(1e-3f, col.radius * std::max(scale.x, scale.y))};
            out.shapes.push_back(b2CreateCircleShape(out.body, &sd, &circle));
            break;
        }
        case Collider2DComponent::Shape::Capsule: {
            const float r = std::max(1e-3f, col.size.x * scale.x * 0.5f);
            const float half = std::max(0.0f, col.size.y * scale.y * 0.5f - r);
            if (half <= 1e-4f) {
                const b2Circle circle{off, r};
                out.shapes.push_back(b2CreateCircleShape(out.body, &sd, &circle));
            } else {
                const b2Capsule cap{{off.x, off.y - half}, {off.x, off.y + half}, r};
                out.shapes.push_back(b2CreateCapsuleShape(out.body, &sd, &cap));
            }
            break;
        }
        case Collider2DComponent::Shape::Polygon:
        case Collider2DComponent::Shape::Chain: {
            std::vector<math::Vec2> pts;
            pts.reserve(col.points.size());
            for (const math::Vec2 q : col.points) pts.push_back({q.x * scale.x + off.x, q.y * scale.y + off.y});
            const bool dynamic = bd.type == b2_dynamicBody;
            if (col.shape == Collider2DComponent::Shape::Chain && !dynamic && !col.is_trigger) {
                create_chain(e, col, sd, std::move(pts), scale.x * scale.y < 0, out);
                break;
            }
            if (col.shape == Collider2DComponent::Shape::Chain && warned_.insert(e.bits()).second)
                VICTUS_LOG_WARN("physics", "entity {}: a chain Collider2D on a {} has no mass or sensor; filling its outline "
                                           "as a polygon", e.index, col.is_trigger ? "trigger" : "dynamic body");
            create_polygon(e, sd, pts, out);
            break;
        }
        }
        if (out.shapes.empty()) {
            // Invalid outline: nothing to collide with, and no body (queries, mass) either.
            b2DestroyBody(out.body);
            out.body = {};
        }
        if (bd.type == b2_dynamicBody && rb && b2Body_IsValid(out.body)) {
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

    void create_polygon(Entity e, const b2ShapeDef& sd, const std::vector<math::Vec2>& pts, Entry& out) {
        auto pieces = decompose_polygon(pts, B2_MAX_POLYGON_VERTICES);
        if (!pieces) {
            if (warned_.insert(e.bits()).second)
                VICTUS_LOG_WARN("physics", "entity {}: Collider2D {}; no body created", e.index, pieces.error().message);
            return;
        }
        for (const auto& piece : *pieces) {
            b2Vec2 v[B2_MAX_POLYGON_VERTICES];
            for (std::size_t i = 0; i < piece.size(); ++i) v[i] = {piece[i].x, piece[i].y};
            const b2Hull hull = b2ComputeHull(v, static_cast<int>(piece.size()));
            if (hull.count == 0) continue; // a sliver below Box2D's linear slop: its neighbours cover it
            const b2Polygon poly = b2MakePolygon(&hull, 0.0f);
            out.shapes.push_back(b2CreatePolygonShape(out.body, &sd, &poly));
        }
        if (out.shapes.empty() && warned_.insert(e.bits()).second)
            VICTUS_LOG_WARN("physics", "entity {}: Collider2D polygon is too small to collide; no body created", e.index);
    }

    // Box2D chains collide on the right of their direction. VICTUS faces open chains to the left of
    // the authored direction (local "up" for ground drawn left to right, kept under mirroring) and
    // loops outward, or inward with `inside`.
    void create_chain(Entity e, const Collider2DComponent& col, const b2ShapeDef& sd, std::vector<math::Vec2> pts,
                      bool mirrored, Entry& out) {
        // Box2D needs points further apart than its linear slop (5 mm).
        constexpr float kMinGap = 0.0075f;
        std::vector<b2Vec2> v;
        v.reserve(pts.size());
        for (const math::Vec2 q : pts) {
            if (!std::isfinite(q.x) || !std::isfinite(q.y)) {
                v.clear();
                break;
            }
            if (!v.empty() && std::hypot(q.x - v.back().x, q.y - v.back().y) < kMinGap) continue;
            v.push_back({q.x, q.y});
        }
        if (col.loop && v.size() > 1 && std::hypot(v.front().x - v.back().x, v.front().y - v.back().y) < kMinGap) v.pop_back();
        const std::size_t need = col.loop ? 3 : 2;
        if (v.size() < need || self_intersects(pts, col.loop)) {
            if (warned_.insert(e.bits()).second)
                VICTUS_LOG_WARN("physics", "entity {}: Collider2D chain needs at least {} distinct points that do not cross; "
                                           "no body created", e.index, need);
            return;
        }
        bool reverse = false;
        if (col.loop) {
            float area = 0;
            for (std::size_t i = 0; i < v.size(); ++i) area += b2Cross(v[i], v[(i + 1) % v.size()]);
            reverse = col.inside ? area > 0 : area < 0; // outward = counter-clockwise
        } else {
            reverse = !mirrored;
        }
        if (reverse) std::reverse(v.begin(), v.end());
        b2SurfaceMaterial material = sd.material;
        b2ChainDef cd = b2DefaultChainDef();
        cd.points = v.data();
        cd.pointCount = static_cast<int>(v.size());
        cd.isLoop = col.loop;
        cd.materials = &material;
        cd.materialCount = 1;
        cd.filter = sd.filter;
        cd.enableSensorEvents = true;
        if (!col.loop) {
            // Ghost points continue the end segments straight, so bodies slide off the ends smoothly.
            const b2Vec2 a = v[0], b = v[1], y = v[v.size() - 2], z = v.back();
            cd.ghost1 = b2Sub(a, b2Sub(b, a));
            cd.ghost2 = b2Add(z, b2Sub(z, y));
        }
        const b2ChainId chain = b2CreateChain(out.body, &cd);
        out.shapes.resize(static_cast<std::size_t>(b2Chain_GetSegmentCount(chain)));
        const int n = b2Chain_GetSegments(chain, out.shapes.data(), static_cast<int>(out.shapes.size()));
        out.shapes.resize(static_cast<std::size_t>(std::max(0, n)));
    }

    struct Compound {
        b2BodyId body{};
        std::vector<b2ShapeId> shapes;
        std::uint64_t revision{0};
        math::Vec2 scale{1, 1};
        math::Vec2 position;
        float angle{0};
        std::uint64_t seen{0};
    };

    void create_compound(Entity e, const StaticShapes2DComponent& c, math::Vec2 p, float angle, math::Vec2 scale,
                         Compound& out) {
        out.revision = c.revision;
        out.scale = scale;
        out.position = p;
        out.angle = angle;
        if (c.boxes.empty()) return;
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        bd.position = v2(p);
        bd.rotation = b2MakeRot(angle);
        out.body = b2CreateBody(world_, &bd);
        const std::uint32_t layer = std::min<std::uint32_t>(c.layer, 15u);
        out.shapes.reserve(c.boxes.size());
        for (const auto& box : c.boxes) {
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.material.friction = std::max(0.0f, c.friction);
            sd.material.restitution = std::clamp(c.restitution, 0.0f, 1.0f);
            sd.enableSensorEvents = true;
            sd.enableContactEvents = true;
            sd.enablePreSolveEvents = box.one_way;
            if (box.one_way) sd.userData = reinterpret_cast<void*>(kOneWay);
            sd.filter.categoryBits = 1ull << layer;
            sd.filter.maskBits = static_cast<std::uint64_t>(c.mask & matrix_[layer] & 0xFFFFu);
            const float hx = std::max(1e-3f, (box.max.x - box.min.x) * scale.x * 0.5f);
            const float hy = std::max(1e-3f, (box.max.y - box.min.y) * scale.y * 0.5f);
            const b2Vec2 centre{(box.min.x + box.max.x) * 0.5f * scale.x, (box.min.y + box.max.y) * 0.5f * scale.y};
            const b2Polygon poly = b2MakeOffsetBox(hx, hy, centre, b2MakeRot(0));
            const b2ShapeId id = b2CreatePolygonShape(out.body, &sd, &poly);
            out.shapes.push_back(id);
            shapes_[b2StoreShapeId(id)] = e;
        }
    }

    void destroy_compound(Compound& c) {
        if (!b2Body_IsValid(c.body)) return;
        for (const b2ShapeId s : c.shapes) retired_.push_back(b2StoreShapeId(s));
        c.shapes.clear();
        b2DestroyBody(c.body);
        c.body = {};
    }

    void sync_compounds(scene::Scene& scene) {
        scene.world().each<StaticShapes2DComponent>([&](Entity e, StaticShapes2DComponent& c) {
            const Pose pose = world_pose(scene, e);
            const math::Vec3 scale = abs(pose.scale);
            const math::Vec2 p{pose.position.x, pose.position.y};
            const float angle = z_angle(pose.rotation);
            auto it = compounds_.find(e.bits());
            if (it != compounds_.end() &&
                (it->second.revision != c.revision || std::abs(it->second.scale.x - scale.x) > 1e-4f ||
                 std::abs(it->second.scale.y - scale.y) > 1e-4f)) {
                destroy_compound(it->second);
                compounds_.erase(it);
                it = compounds_.end();
            }
            if (it == compounds_.end()) {
                Compound k;
                create_compound(e, c, p, angle, {scale.x, scale.y}, k);
                it = compounds_.emplace(e.bits(), std::move(k)).first;
            }
            Compound& k = it->second;
            k.seen = generation_;
            if (std::abs(p.x - k.position.x) > 1e-5f || std::abs(p.y - k.position.y) > 1e-5f ||
                std::abs(angle - k.angle) > 1e-5f) {
                if (b2Body_IsValid(k.body)) b2Body_SetTransform(k.body, v2(p), b2MakeRot(angle));
                k.position = p;
                k.angle = angle;
            }
        });
        for (auto it = compounds_.begin(); it != compounds_.end();) {
            if (it->second.seen != generation_) {
                destroy_compound(it->second);
                it = compounds_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void destroy(Entry& b) {
        if (!b2Body_IsValid(b.body)) return;
        // Keep the shape -> entity mapping until the next event pass so end events still resolve.
        for (const b2ShapeId s : b.shapes) retired_.push_back(b2StoreShapeId(s));
        b.shapes.clear();
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

    // ---- 2D characters ----------------------------------------------------------------------------

    struct Character {
        b2BodyId body{};
        b2ShapeId shape{};
        CharacterController2DComponent comp;
        b2Vec2 position{};  // capsule centre (= entity origin) as last written to the Transform
        float z{0};
        math::Quat rotation3d;
        b2Vec2 velocity{};
        float desired{0};   // walking speed along world X
        float desired_y{0}; // top-down characters: speed along world Y
        float jump{0};
        float drop{0};      // seconds left ignoring one-way platforms
        bool grounded{false}, steep{false}, on_one_way{false};
        b2Vec2 ground_normal{0, 1};
        Entity ground;
        b2BodyId ground_body{};
        std::uint64_t seen{0};
        [[nodiscard]] float radius() const { return std::max(0.05f, comp.size.x * 0.5f); }
        [[nodiscard]] float half() const { return std::max(0.01f, comp.size.y * 0.5f - radius()); }
        [[nodiscard]] b2QueryFilter filter(const std::uint32_t* matrix) const {
            const std::uint32_t layer = std::min<std::uint32_t>(comp.layer, 15u);
            return {1ull << layer, static_cast<std::uint64_t>(comp.mask & matrix[layer] & 0xFFFFu)};
        }
    };

    // Planes gathered around a mover capsule (origin relative, as returned by b2World_CollideMover).
    struct MoverPlanes {
        static constexpr int kCapacity = 16;
        b2CollisionPlane planes[kCapacity]{};
        b2ShapeId shapes[kCapacity]{};
        b2Vec2 points[kCapacity]{};
        int count{0};
        b2Vec2 origin{};
    };

    struct MoverHit {
        float fraction{1.0f};
        b2Vec2 normal{};
        b2ShapeId shape{};
    };

    // Collide skin: planes are gathered with a slightly larger capsule than the one that is cast, so the
    // mover rests a hair away from surfaces and keeps sliding along them instead of sticking.
    static constexpr float kSkin = 0.005f;

    static bool accept_one_way(const Character& c, b2ShapeId shape, b2Vec2 normal, b2Vec2 origin) {
        if (c.drop > 0) return false;
        const b2Vec2 up = up_of(shape);
        if (b2Dot(normal, up) < 0.7f) return false;
        // Only a platform surface below the lower hemisphere centre supports the character: while the capsule
        // is still partly below the top surface the platform is ignored and the character keeps passing.
        const b2Vec2 lower{origin.x, origin.y - c.half()};
        const b2Vec2 surface = f2(b2Shape_GetClosestPoint(shape, pos(lower)));
        return b2Dot(b2Sub(lower, surface), up) > 0.4f * c.radius();
    }

    void collide_mover(const Character& c, b2Vec2 origin, MoverPlanes& out) const {
        out.count = 0;
        out.origin = origin;
        const b2Capsule cap{{0, -c.half()}, {0, c.half()}, c.radius() + kSkin};
        struct Ctx {
            const Character* c;
            MoverPlanes* out;
            b2Vec2 origin;
        } ctx{&c, &out, origin};
        b2World_CollideMover(
            world_, pos(origin), &cap, c.filter(matrix_),
            [](b2ShapeId shape, const b2PlaneResult* r, void* user) -> bool {
                auto& x = *static_cast<Ctx*>(user);
                if (B2_ID_EQUALS(shape, x.c->shape) || b2Shape_IsSensor(shape)) return true;
                if ((flags_of(shape) & kOneWay) && !accept_one_way(*x.c, shape, r->plane.normal, x.origin)) return true;
                MoverPlanes& o = *x.out;
                if (o.count >= MoverPlanes::kCapacity) return false;
                o.planes[o.count] = {r->plane, FLT_MAX, 0.0f, true};
                o.shapes[o.count] = shape;
                // Contact point on the surface (origin relative): the shape's closest point to the end of the
                // mover's core segment that faces it.
                const b2Vec2 n = r->plane.normal;
                const float hl = x.c->half();
                const float dl = -hl * n.y, dh = hl * n.y;
                const b2Vec2 core = std::abs(dl - dh) < 1e-3f ? b2Vec2{0, 0} : b2Vec2{0, dl < dh ? -hl : hl};
                const b2Vec2 at{x.origin.x + core.x, x.origin.y + core.y};
                o.points[o.count] = b2Sub(f2(b2Shape_GetClosestPoint(shape, pos(at))), x.origin);
                ++o.count;
                return true;
            },
            &ctx);
    }

    MoverHit cast_mover(const Character& c, b2Vec2 origin, b2Vec2 delta) const {
        MoverHit hit;
        if (b2LengthSquared(delta) < 1e-12f) return hit;
        const b2Vec2 pts[2] = {{0, -c.half()}, {0, c.half()}};
        const b2ShapeProxy proxy = b2MakeProxy(pts, 2, c.radius());
        struct Ctx {
            const Character* c;
            b2Vec2 delta;
            MoverHit* hit;
        } ctx{&c, delta, &hit};
        b2World_CastShape(
            world_, pos(origin), &proxy, delta, c.filter(matrix_),
            [](b2ShapeId shape, b2Pos, b2Vec2 normal, float fraction, void* user) -> float {
                auto& x = *static_cast<Ctx*>(user);
                if (B2_ID_EQUALS(shape, x.c->shape) || b2Shape_IsSensor(shape)) return -1.0f;
                if (fraction <= 0.0f) return -1.0f; // already touching: the plane solver handles it
                if (flags_of(shape) & kOneWay) {
                    const b2Vec2 up = up_of(shape);
                    if (x.c->drop > 0 || b2Dot(normal, up) < 0.7f || b2Dot(x.delta, up) >= 0) return -1.0f;
                }
                if (fraction < x.hit->fraction) {
                    x.hit->fraction = fraction;
                    x.hit->normal = normal;
                    x.hit->shape = shape;
                }
                return fraction;
            },
            &ctx);
        return hit;
    }

    void sync_characters(scene::Scene& scene) {
        scene.world().each<CharacterController2DComponent>([&](Entity e, CharacterController2DComponent& comp) {
            const Pose pose = world_pose(scene, e);
            b2Vec2 p{pose.position.x, pose.position.y};
            auto it = chars_.find(e.bits());
            b2Vec2 velocity{};
            float desired = 0, desired_y = 0;
            if (it != chars_.end() && !(it->second.comp == comp)) {
                velocity = it->second.velocity;
                desired = it->second.desired;
                desired_y = it->second.desired_y;
                // Resizing keeps the feet where they were instead of burying the new capsule in the ground.
                const Character& old = it->second;
                Character resized;
                resized.comp = comp;
                const float grow = (resized.half() + resized.radius()) - (old.half() + old.radius());
                if (old.grounded && std::abs(grow) > 1e-5f &&
                    std::abs(p.x - old.position.x) <= 1e-4f && std::abs(p.y - old.position.y) <= 1e-4f) {
                    p.y += grow;
                    write_world_pose(scene, e, {p.x, p.y, pose.position.z}, pose.rotation);
                }
                destroy_character(it->second);
                chars_.erase(it);
                it = chars_.end();
            }
            if (it == chars_.end()) {
                Character c;
                create_character(e, comp, p, c);
                c.velocity = velocity;
                c.desired = desired;
                c.desired_y = desired_y;
                it = chars_.emplace(e.bits(), c).first;
            }
            Character& c = it->second;
            c.seen = generation_;
            c.z = pose.position.z;
            c.rotation3d = pose.rotation;
            if (std::abs(p.x - c.position.x) > 1e-4f || std::abs(p.y - c.position.y) > 1e-4f) {
                b2Body_SetTransform(c.body, pos(p), b2Rot_identity); // gameplay teleported the character
                c.position = p;
                c.grounded = false;
            }
        });
        for (auto it = chars_.begin(); it != chars_.end();) {
            if (it->second.seen != generation_) {
                destroy_character(it->second);
                it = chars_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void create_character(Entity e, const CharacterController2DComponent& comp, b2Vec2 p, Character& out) {
        out.comp = comp;
        out.position = p;
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_kinematicBody; // a twin so raycasts, triggers and dynamic bodies see the character
        bd.position = pos(p);
        bd.motionLocks.angularZ = true;
        out.body = b2CreateBody(world_, &bd);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.material.friction = 0.0f;
        sd.enableSensorEvents = true;
        sd.enableContactEvents = true;
        sd.userData = reinterpret_cast<void*>(kCharacter);
        sd.filter.categoryBits = out.filter(matrix_).categoryBits;
        sd.filter.maskBits = out.filter(matrix_).maskBits;
        const b2Capsule cap{{0, -out.half()}, {0, out.half()}, out.radius()};
        out.shape = b2CreateCapsuleShape(out.body, &sd, &cap);
        shapes_[b2StoreShapeId(out.shape)] = e;
    }

    void destroy_character(Character& c) {
        if (!b2Body_IsValid(c.body)) return;
        retired_.push_back(b2StoreShapeId(c.shape));
        b2DestroyBody(c.body);
        c.body = {};
    }

    b2Vec2 ground_velocity(const Character& c) const {
        if (!c.grounded || B2_IS_NULL(c.ground_body) || !b2Body_IsValid(c.ground_body)) return {0, 0};
        const b2Vec2 foot{c.position.x, c.position.y - c.half() - c.radius()};
        return b2Body_GetWorldPointVelocity(c.ground_body, pos(foot));
    }

    // Kinematic movement, following Box2D's geometric mover (collide planes -> solve -> cast) with the
    // same velocity model as the 3D CharacterController so both feel alike.
    void update_characters(scene::Scene& scene, float dt) {
        if (chars_.empty() || dt <= 0) return;
        VICTUS_PROFILE_SCOPE("physics.characters2d");
        const b2Vec2 world_gravity = b2World_GetGravity(world_);
        MoverPlanes planes;
        for (auto& [key, c] : chars_) {
            const CharacterController2DComponent& comp = c.comp;
            const b2Vec2 g = comp.top_down ? b2Vec2{0, 0} : b2MulSV(comp.gravity_scale, world_gravity);
            const float cos_slope = std::cos(radians(std::clamp(comp.max_slope, 1.0f, 89.0f)));
            c.drop = std::max(0.0f, c.drop - dt);
            const b2Vec2 ground_v = ground_velocity(c);
            b2Vec2 v = c.velocity;
            const bool on_ground = c.grounded && v.y - ground_v.y < 0.1f;
            const bool jumped = on_ground && c.jump > 0;
            if (comp.top_down) {
                // Top-down: walk in the plane with the same responsiveness on both axes, no gravity,
                // no ground and no jumping.
                v = {c.desired, c.desired_y};
            } else if (on_ground) {
                v = {ground_v.x + c.desired, ground_v.y}; // ride moving platforms
                if (jumped) v.y += c.jump;
            } else {
                const float k = comp.air_control >= 1.0f ? 1.0f
                                                         : 1.0f - std::exp(-std::max(0.0f, comp.air_control) * 12.0f * dt);
                v.x += (c.desired - v.x) * k;
            }
            c.jump = 0;
            const bool planted = on_ground && !jumped;
            // On walkable ground only the part of gravity pressing into the surface applies, so characters
            // stand still on slopes up to max_slope instead of creeping downhill.
            if (planted && !comp.top_down) v = b2MulAdd(v, b2Dot(c.ground_normal, g) * dt, c.ground_normal);
            else v = b2MulAdd(v, dt, g); // g is zero for top-down characters

            b2Vec2 p = c.position;
            const b2Vec2 target = b2MulAdd(p, dt, v);
            for (int iteration = 0; iteration < 5; ++iteration) {
                collide_mover(c, p, planes);
                const b2PlaneSolverResult solved = b2SolvePlanes(b2Sub(target, p), planes.planes, planes.count);
                const MoverHit hit = cast_mover(c, p, solved.delta);
                const b2Vec2 d = b2MulSV(hit.fraction, solved.delta);
                p = b2Add(p, d);
                if (b2LengthSquared(d) < 1e-6f) break;
            }
            if (comp.push_force > 0) push_bodies(planes, v, comp.push_force * dt);
            v = b2ClipVector(v, planes.planes, planes.count);

            // Ground: probe just below the capsule. Walking characters snap down onto ground within reach.
            const auto probe = [&](b2Vec2 at) {
                collide_mover(c, b2Vec2{at.x, at.y - 0.02f}, planes);
                int best = -1;
                c.steep = false;
                for (int i = 0; i < planes.count; ++i) {
                    const float ny = planes.planes[i].plane.normal.y;
                    if (ny >= cos_slope && (best < 0 || ny > planes.planes[best].plane.normal.y)) best = i;
                    else if (ny > 0.05f && ny < cos_slope && planes.points[i].y < 0) c.steep = true;
                }
                return best;
            };
            int best = comp.top_down ? -1 : probe(p);
            if (comp.top_down) c.steep = false;
            if (best < 0 && planted && comp.snap_distance > 0) {
                const MoverHit down = cast_mover(c, p, {0, -comp.snap_distance});
                if (down.fraction < 1.0f && down.normal.y >= cos_slope) {
                    p.y -= comp.snap_distance * down.fraction;
                    best = probe(p);
                }
            }
            c.grounded = false;
            c.on_one_way = false;
            c.ground = {};
            c.ground_body = {};
            if (best >= 0) {
                const b2ShapeId gs = planes.shapes[best];
                c.ground_normal = planes.planes[best].plane.normal;
                c.ground = entity_of(gs);
                c.ground_body = b2Shape_GetBody(gs);
                c.on_one_way = (flags_of(gs) & kOneWay) != 0;
                c.position = p;
                c.grounded = true;
                const b2Vec2 gv = ground_velocity(c);
                c.grounded = !jumped && v.y - gv.y < 0.1f;
                if (!c.grounded) c.ground_body = {};
                if (c.grounded && v.y < gv.y) v.y = gv.y; // landed: stop falling relative to the ground
            } else if (!c.steep) {
                c.ground_normal = {0, 1};
            }
            c.velocity = v;
            c.position = p;
            b2Body_SetTargetTransform(c.body, b2WorldTransform{pos(p), b2Rot_identity}, dt, true);
            const Entity e{static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32)};
            if (scene.world().alive(e)) write_world_pose(scene, e, {p.x, p.y, c.z}, c.rotation3d);
        }
    }

    // A normal impulse on each dynamic body the mover touches, as if the character had infinite mass,
    // capped at push_force * dt so characters shove crates instead of launching them.
    static void push_bodies(const MoverPlanes& planes, b2Vec2 v, float max_impulse) {
        for (int i = 0; i < planes.count; ++i) {
            const b2BodyId body = b2Shape_GetBody(planes.shapes[i]);
            if (b2Body_GetType(body) != b2_dynamicBody) continue;
            const b2Vec2 point = b2Add(planes.origin, planes.points[i]);
            const b2Vec2 normal = b2Neg(planes.planes[i].plane.normal); // into the body
            const float mass = b2Body_GetMass(body), inertia = b2Body_GetRotationalInertia(body);
            const float inv_m = mass > 0 ? 1.0f / mass : 0.0f, inv_i = inertia > 0 ? 1.0f / inertia : 0.0f;
            const b2Vec2 r = b2Sub(point, f2(b2Body_GetWorldCenter(body)));
            const float rn = b2Cross(r, normal);
            const float k = inv_m + inv_i * rn * rn;
            if (k <= 0) continue;
            const b2Vec2 vb = b2Add(b2Body_GetLinearVelocity(body), b2CrossSV(b2Body_GetAngularVelocity(body), r));
            const float vn = b2Dot(b2Sub(vb, v), normal);
            const float impulse = std::clamp(-vn / k, 0.0f, max_impulse);
            if (impulse > 0) b2Body_ApplyLinearImpulse(body, b2MulSV(impulse, normal), pos(point), true);
        }
    }

    // ---- 2D joints --------------------------------------------------------------------------------

    struct Joint {
        b2JointId id{};
        Joint2DComponent comp;
        b2BodyId a{}, b{};  // a = connected body or the shared static anchor body, b = this entity
        bool broken{false};
        float force{0};
        std::uint64_t seen{0};
    };

    b2BodyId anchor_body() {
        if (B2_IS_NULL(anchor_) || !b2Body_IsValid(anchor_)) {
            b2BodyDef bd = b2DefaultBodyDef();
            anchor_ = b2CreateBody(world_, &bd);
        }
        return anchor_;
    }

    void sync_joints(scene::Scene& scene) {
        scene.world().each<Joint2DComponent>([&](Entity e, Joint2DComponent& comp) {
            const auto ia = bodies_.find(e.bits());
            const Entry* other = nullptr;
            bool resolved = ia != bodies_.end();
            if (resolved && !comp.connected.is_nil()) {
                const Entity oe = scene.find(comp.connected);
                const auto ib = oe.is_null() ? bodies_.end() : bodies_.find(oe.bits());
                if (ib == bodies_.end() || oe == e) resolved = false;
                else other = &ib->second;
            }
            // A joint needs at least one simulated body.
            if (resolved && ia->second.type != b2_dynamicBody && (!other || other->type != b2_dynamicBody)) resolved = false;
            auto it = joints_.find(e.bits());
            if (!resolved) {
                if (it != joints_.end()) {
                    destroy_joint(it->second);
                    joints_.erase(it);
                }
                return;
            }
            const b2BodyId a = other ? other->body : anchor_body(), b = ia->second.body;
            if (it != joints_.end()) {
                Joint& j = it->second;
                Joint2DComponent x = j.comp;
                x.motor = comp.motor;
                x.motor_speed = comp.motor_speed;
                x.motor_force = comp.motor_force;
                const bool lost = !j.broken && !b2Joint_IsValid(j.id); // a body was rebuilt
                if (lost || !B2_ID_EQUALS(j.a, a) || !B2_ID_EQUALS(j.b, b) || !(x == comp)) {
                    destroy_joint(j);
                    joints_.erase(it);
                    it = joints_.end();
                } else if (!(j.comp == comp)) {
                    j.comp = comp;
                    apply_motor(j);
                }
            }
            if (it == joints_.end()) {
                Joint j;
                j.comp = comp;
                j.a = a;
                j.b = b;
                create_joint(scene, e, j);
                it = joints_.emplace(e.bits(), j).first;
            }
            it->second.seen = generation_;
        });
        for (auto it = joints_.begin(); it != joints_.end();) {
            if (it->second.seen != generation_) {
                destroy_joint(it->second);
                it = joints_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void create_joint(scene::Scene& scene, Entity e, Joint& j) {
        const Joint2DComponent& c = j.comp;
        const math::Mat4 m = scene.world_matrix(e);
        const math::Vec3 w3 = m.transform_point({c.anchor.x, c.anchor.y, 0});
        const b2Vec2 anchor{w3.x, w3.y};
        const math::Vec3 ax3 = m.transform_point({c.axis.x, c.axis.y, 0}) - m.transform_point({});
        b2Vec2 axis{ax3.x, ax3.y};
        axis = b2Length(axis) > 1e-6f ? b2Normalize(axis) : b2Vec2{1, 0};
        const b2Rot q = b2MakeRotFromUnitVector(axis);
        // Body A is the connected body (or the world) and B this entity, so angles and translations measure
        // this entity's motion relative to what it is attached to.
        const auto frame = [&](b2BodyId body, b2Vec2 world_point) {
            return b2Transform{b2Body_GetLocalPoint(body, pos(world_point)), b2InvMulRot(b2Body_GetRotation(body), q)};
        };
        const auto fill = [&](b2JointDef& d) {
            d.bodyIdA = j.a;
            d.bodyIdB = j.b;
            d.localFrameA = frame(j.a, anchor);
            d.localFrameB = frame(j.b, anchor);
            d.collideConnected = c.collide_connected;
        };
        const float lo = std::min(c.min_limit, c.max_limit), hi = std::max(c.min_limit, c.max_limit);
        const float hertz = std::max(0.0f, c.spring_hertz), damping = std::max(0.0f, c.spring_damping);
        switch (c.type) {
        case Joint2DComponent::Type::Weld: {
            b2WeldJointDef d = b2DefaultWeldJointDef();
            fill(d.base);
            d.linearHertz = d.angularHertz = c.spring ? hertz : 0.0f; // 0 = rigid
            d.linearDampingRatio = d.angularDampingRatio = damping;
            j.id = b2CreateWeldJoint(world_, &d);
            break;
        }
        case Joint2DComponent::Type::Revolute: {
            b2RevoluteJointDef d = b2DefaultRevoluteJointDef();
            fill(d.base);
            constexpr float kMax = 0.99f * std::numbers::pi_v<float>;
            d.enableLimit = c.limits;
            d.lowerAngle = std::clamp(radians(lo), -kMax, kMax);
            d.upperAngle = std::clamp(radians(hi), d.lowerAngle, kMax);
            d.enableSpring = c.spring;
            d.hertz = hertz;
            d.dampingRatio = damping;
            j.id = b2CreateRevoluteJoint(world_, &d);
            break;
        }
        case Joint2DComponent::Type::Prismatic: {
            b2PrismaticJointDef d = b2DefaultPrismaticJointDef();
            fill(d.base);
            d.enableLimit = c.limits;
            d.lowerTranslation = lo;
            d.upperTranslation = hi;
            d.enableSpring = c.spring;
            d.hertz = hertz;
            d.dampingRatio = damping;
            j.id = b2CreatePrismaticJoint(world_, &d);
            break;
        }
        case Joint2DComponent::Type::Distance: {
            b2DistanceJointDef d = b2DefaultDistanceJointDef();
            fill(d.base);
            b2Vec2 other_point{c.connected_anchor.x, c.connected_anchor.y};
            if (!c.connected.is_nil()) {
                const math::Vec3 p = scene.world_matrix(scene.find(c.connected)).transform_point({other_point.x, other_point.y, 0});
                other_point = {p.x, p.y};
            }
            d.base.localFrameA.p = b2Body_GetLocalPoint(j.a, pos(other_point));
            d.length = std::max(0.01f, b2Distance(other_point, anchor));
            d.enableLimit = c.limits;
            if (c.limits) {
                // Rope-like: free between the limits unless a spring is requested.
                d.minLength = std::max(0.01f, lo);
                d.maxLength = std::max(d.minLength, hi);
                d.enableSpring = true;
                d.hertz = c.spring ? hertz : 0.0f;
            } else {
                d.enableSpring = c.spring;
                d.hertz = hertz;
            }
            d.dampingRatio = damping;
            j.id = b2CreateDistanceJoint(world_, &d);
            break;
        }
        case Joint2DComponent::Type::Wheel: {
            b2WheelJointDef d = b2DefaultWheelJointDef();
            fill(d.base);
            d.enableSpring = true;
            d.hertz = hertz;
            d.dampingRatio = damping;
            d.enableLimit = c.limits;
            d.lowerTranslation = lo;
            d.upperTranslation = hi;
            j.id = b2CreateWheelJoint(world_, &d);
            break;
        }
        }
        apply_motor(j);
    }

    void apply_motor(Joint& j) {
        if (j.broken || !b2Joint_IsValid(j.id)) return;
        const Joint2DComponent& c = j.comp;
        const float force = std::max(0.0f, c.motor_force);
        switch (c.type) {
        case Joint2DComponent::Type::Revolute:
            b2RevoluteJoint_EnableMotor(j.id, c.motor);
            b2RevoluteJoint_SetMaxMotorTorque(j.id, force);
            b2RevoluteJoint_SetTargetMotorSpeed(j.id, radians(c.motor_speed));
            break;
        case Joint2DComponent::Type::Wheel:
            b2WheelJoint_EnableMotor(j.id, c.motor);
            b2WheelJoint_SetMaxMotorTorque(j.id, force);
            b2WheelJoint_SetTargetMotorSpeed(j.id, radians(c.motor_speed));
            break;
        case Joint2DComponent::Type::Prismatic:
            b2PrismaticJoint_EnableMotor(j.id, c.motor);
            b2PrismaticJoint_SetMaxMotorForce(j.id, force);
            b2PrismaticJoint_SetTargetMotorSpeed(j.id, c.motor_speed);
            break;
        case Joint2DComponent::Type::Distance:
            b2DistanceJoint_EnableMotor(j.id, c.motor);
            b2DistanceJoint_SetMaxMotorForce(j.id, force);
            b2DistanceJoint_SetTargetMotorSpeed(j.id, c.motor_speed);
            break;
        case Joint2DComponent::Type::Weld: return;
        }
        b2Joint_WakeBodies(j.id);
    }

    static void destroy_joint(Joint& j) {
        if (b2Joint_IsValid(j.id)) b2DestroyJoint(j.id);
        j.id = {};
    }

    // Reaction of the last step (N, or N*m for torque-only reactions) and breaking.
    void check_joints() {
        for (auto& [key, j] : joints_) {
            if (j.broken || !b2Joint_IsValid(j.id)) continue;
            j.force = std::max(b2Length(b2Joint_GetConstraintForce(j.id)), std::abs(b2Joint_GetConstraintTorque(j.id)));
            if (j.comp.break_force > 0 && j.force > j.comp.break_force) {
                b2Joint_WakeBodies(j.id);
                b2DestroyJoint(j.id);
                j.id = {};
                j.broken = true; // stays broken until the component changes
            }
        }
    }

    PhysicsSettings settings_;
    std::uint32_t matrix_[kMaxLayers]{};
    TaskPool pool_;
    b2WorldId world_{};
    std::unordered_map<std::uint64_t, Entry> bodies_;
    // Colliders whose outline cannot be built: remembered so they are not rebuilt (and re-logged)
    // every step until they change.
    struct Failed {
        Collider2DComponent collider;
        bool has_rb{false};
        RigidBody2DComponent rb;
        math::Vec2 scale{1, 1};
        std::uint64_t seen{0};
    };
    std::unordered_map<std::uint64_t, Failed> failed_;
    std::unordered_set<std::uint64_t> warned_;
    std::unordered_map<std::uint64_t, Compound> compounds_;
    std::unordered_map<std::uint64_t, Entity> shapes_;
    std::unordered_map<std::uint64_t, Character> chars_;
    std::unordered_map<std::uint64_t, Joint> joints_;
    b2BodyId anchor_{};
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
