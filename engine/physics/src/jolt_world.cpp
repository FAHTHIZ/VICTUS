// SPDX-License-Identifier: Apache-2.0
// 3D physics backend on Jolt Physics 5.7.0 (MIT). Jolt types never leave this file.
#include "internal.hpp"

#include "victus/core/jobs.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace victus::physics::detail {

namespace {

// ---- process-wide Jolt registration (reference counted) ----------------------------------------

std::mutex g_jolt_mutex;
int g_jolt_users = 0;

void jolt_trace(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    VICTUS_LOG_DEBUG("physics", "jolt: {}", buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool jolt_assert(const char* expr, const char* msg, const char* file, JPH::uint line) {
    VICTUS_LOG_ERROR("physics", "jolt assert {}:{}: {} {}", file, line, expr, msg ? msg : "");
    return false; // do not break into the debugger
}
#endif

void acquire_jolt() {
    std::lock_guard lock(g_jolt_mutex);
    if (g_jolt_users++ > 0) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = jolt_trace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void release_jolt() {
    std::lock_guard lock(g_jolt_mutex);
    if (--g_jolt_users > 0) return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// ---- layers -------------------------------------------------------------------------------------
// ObjectLayer (32 bits): [0..3] layer index, [4..19] collision mask, [20] moving.
constexpr JPH::BroadPhaseLayer kBpStatic{0};
constexpr JPH::BroadPhaseLayer kBpMoving{1};

constexpr JPH::ObjectLayer make_layer(std::uint32_t layer, std::uint32_t mask, bool moving) {
    return static_cast<JPH::ObjectLayer>((layer & 15u) | ((mask & 0xFFFFu) << 4) | (moving ? (1u << 20) : 0u));
}
constexpr std::uint32_t layer_index(JPH::ObjectLayer l) { return l & 15u; }
constexpr std::uint32_t layer_mask(JPH::ObjectLayer l) { return (l >> 4) & 0xFFFFu; }
constexpr bool layer_moving(JPH::ObjectLayer l) { return ((l >> 20) & 1u) != 0; }

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return layer_moving(l) ? kBpMoving : kBpStatic;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return l == kBpMoving ? "moving" : "static";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l, JPH::BroadPhaseLayer bp) const override {
        return layer_moving(l) || bp == kBpMoving;
    }
};

class LayerPairs final : public JPH::ObjectLayerPairFilter {
public:
    explicit LayerPairs(const std::uint32_t* matrix) { std::copy(matrix, matrix + kMaxLayers, matrix_); }
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (!layer_moving(a) && !layer_moving(b)) return false;
        const std::uint32_t la = layer_index(a), lb = layer_index(b);
        return (layer_mask(a) & (1u << lb)) && (layer_mask(b) & (1u << la)) && (matrix_[la] & (1u << lb)) &&
               (matrix_[lb] & (1u << la));
    }

private:
    std::uint32_t matrix_[kMaxLayers]{};
};

// Query filters: a layer mask on the object layer plus an optional ignored body; triggers are
// never returned by queries (matches Box2D, where sensors do not participate in queries).
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryLayerFilter(std::uint32_t mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer l) const override { return (mask_ & (1u << layer_index(l))) != 0; }

private:
    std::uint32_t mask_;
};
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(JPH::BodyID ignore) : ignore_(ignore) {}
    bool ShouldCollide(const JPH::BodyID& id) const override { return id != ignore_; }
    bool ShouldCollideLocked(const JPH::Body& b) const override { return !b.IsSensor(); }

private:
    JPH::BodyID ignore_;
};

// ---- Jolt job system on the VICTUS work-stealing pool -------------------------------------------

class VictusJobs final : public JPH::JobSystemWithBarrier {
public:
    VictusJobs(jobs::JobSystem& jobs, JPH::uint max_jobs, JPH::uint max_barriers) : jobs_(jobs) {
        JobSystemWithBarrier::Init(max_barriers);
        free_.Init(max_jobs, max_jobs);
    }
    ~VictusJobs() override { jobs_.wait(in_flight_); }
    int GetMaxConcurrency() const override { return static_cast<int>(jobs_.worker_count()) + 1; }
    JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn,
                             JPH::uint32 dependencies) override {
        Job* job = allocate(name, color, fn, dependencies);
        JPH::JobHandle handle(job);
        if (dependencies == 0) QueueJob(job);
        return handle;
    }
    void CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::JobHandle* out, JPH::uint32 count,
                    JPH::uint32 dependencies) override {
        for (JPH::uint32 i = 0; i < count; ++i) out[i] = JPH::JobHandle(allocate(name, color, fn, dependencies));
        if (dependencies == 0) QueueJobs(reinterpret_cast<Job**>(out), count);
    }
    void WaitForJobs(Barrier* barrier) override {
        JobSystemWithBarrier::WaitForJobs(barrier);
        // Jolt jobs may have been executed by the barrier; our wrappers still hold references.
        jobs_.wait(in_flight_);
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        jobs_.submit(
            [job] {
                job->Execute();
                job->Release();
            },
            &in_flight_, jobs::Priority::High);
    }
    void QueueJobs(Job** list, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) QueueJob(list[i]);
    }
    void FreeJob(Job* job) override { free_.DestructObject(job); }

private:
    Job* allocate(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 deps) {
        JPH::uint32 index;
        while ((index = free_.ConstructObject(name, color, this, fn, deps)) == decltype(free_)::cInvalidObjectIndex)
            std::this_thread::yield();
        return &free_.Get(index);
    }
    jobs::JobSystem& jobs_;
    jobs::Counter in_flight_;
    JPH::FixedSizeFreeList<Job> free_;
};

// ---- contact events -----------------------------------------------------------------------------

class Contacts final : public JPH::ContactListener {
public:
    explicit Contacts(const JPH::BodyInterface* bodies) : bodies_(bodies) {}
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m,
                        JPH::ContactSettings&) override {
        ContactEvent ev;
        ev.kind = ContactEvent::Kind::Begin;
        ev.a = decode(a.GetUserData());
        ev.b = decode(b.GetUserData());
        ev.trigger = a.IsSensor() || b.IsSensor();
        if (!ev.trigger && m.mRelativeContactPointsOn1.size() > 0) {
            const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
            ev.point = {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()), static_cast<float>(p.GetZ())};
            ev.normal = {m.mWorldSpaceNormal.GetX(), m.mWorldSpaceNormal.GetY(), m.mWorldSpaceNormal.GetZ()};
        }
        std::lock_guard lock(mutex_);
        events_.push_back(ev);
        triggers_[pair_key(a.GetID(), b.GetID())] = ev.trigger;
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        ContactEvent ev;
        ev.kind = ContactEvent::Kind::End;
        // Bodies are locked during this callback: use the no-lock interface (read only).
        ev.a = decode(bodies_->GetUserData(pair.GetBody1ID()));
        ev.b = decode(bodies_->GetUserData(pair.GetBody2ID()));
        std::lock_guard lock(mutex_);
        const auto key = pair_key(pair.GetBody1ID(), pair.GetBody2ID());
        if (auto it = triggers_.find(key); it != triggers_.end()) {
            ev.trigger = it->second;
            triggers_.erase(it);
        }
        if (!ev.a.is_null() && !ev.b.is_null()) events_.push_back(ev);
    }
    void drain(std::vector<ContactEvent>& out) {
        std::lock_guard lock(mutex_);
        out.insert(out.end(), events_.begin(), events_.end());
        events_.clear();
    }
    void reset() {
        std::lock_guard lock(mutex_);
        events_.clear();
        triggers_.clear();
    }

private:
    static std::uint64_t pair_key(JPH::BodyID a, JPH::BodyID b) {
        std::uint32_t x = a.GetIndexAndSequenceNumber(), y = b.GetIndexAndSequenceNumber();
        if (x > y) std::swap(x, y);
        return (static_cast<std::uint64_t>(x) << 32) | y;
    }
    const JPH::BodyInterface* bodies_;
    std::mutex mutex_;
    std::vector<ContactEvent> events_;
    std::unordered_map<std::uint64_t, bool> triggers_;
};

JPH::Vec3 to_jolt(math::Vec3 v) { return {v.x, v.y, v.z}; }
JPH::RVec3 to_jolt_r(math::Vec3 v) { return {v.x, v.y, v.z}; }
JPH::Quat to_jolt(math::Quat q) { return JPH::Quat(q.x, q.y, q.z, q.w).Normalized(); }
math::Vec3 from_jolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
#ifdef JPH_DOUBLE_PRECISION
math::Vec3 from_jolt(JPH::RVec3Arg v) {
    return {static_cast<float>(v.GetX()), static_cast<float>(v.GetY()), static_cast<float>(v.GetZ())};
}
#endif
math::Quat from_jolt(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

// ---- the world ----------------------------------------------------------------------------------

class JoltWorld final : public World3D {
public:
    explicit JoltWorld(const PhysicsSettings& s) : settings_(s), pairs_(s.layer_matrix) {
        acquire_jolt();
        temp_ = std::make_unique<JPH::TempAllocatorImpl>(16u << 20);
        if (s.jobs)
            jobs_ = std::make_unique<VictusJobs>(*s.jobs, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
        else
            jobs_ = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        system_ = std::make_unique<JPH::PhysicsSystem>();
        system_->Init(s.max_bodies, 0, s.max_body_pairs, s.max_contact_constraints, bp_layers_, obj_vs_bp_, pairs_);
        system_->SetGravity(to_jolt(s.gravity));
        contacts_ = std::make_unique<Contacts>(&system_->GetBodyInterfaceNoLock());
        system_->SetContactListener(contacts_.get());
    }
    ~JoltWorld() override {
        clear();
        system_.reset();
        contacts_.reset();
        jobs_.reset();
        temp_.reset();
        release_jolt();
    }

    void sync(scene::Scene& scene, float dt) override {
        VICTUS_PROFILE_SCOPE("physics.sync3d");
        ++generation_;
        auto& bi = system_->GetBodyInterface();
        std::size_t added = 0;
        scene.world().each<ColliderComponent>([&](Entity e, ColliderComponent& col) {
            const RigidBodyComponent* rb = scene.world().get<RigidBodyComponent>(e);
            const Pose pose = world_pose(scene, e);
            const math::Vec3 scale = abs(pose.scale);
            auto it = bodies_.find(e.bits());
            if (it != bodies_.end()) {
                Entry& b = it->second;
                const bool same = b.collider == col && b.has_rb == (rb != nullptr) && (!rb || b.rb == *rb) &&
                                  nearly_equal(b.scale, scale, 1e-4f);
                if (!same) {
                    destroy(b);
                    bodies_.erase(it);
                    it = bodies_.end();
                }
            }
            if (it == bodies_.end()) {
                Entry b;
                if (!create(e, col, rb, pose, scale, b)) return;
                ++added;
                it = bodies_.emplace(e.bits(), b).first;
            }
            Entry& b = it->second;
            b.seen = generation_;
            // Gameplay moved the transform since our last write: teleport or drive it.
            if (b.motion == JPH::EMotionType::Kinematic) {
                if (!nearly_equal(pose.position, b.position) || !nearly_equal(pose.rotation, b.rotation) || b.moving) {
                    bi.MoveKinematic(b.id, to_jolt_r(pose.position), to_jolt(pose.rotation), std::max(dt, 1e-4f));
                    b.moving = !nearly_equal(pose.position, b.position) || !nearly_equal(pose.rotation, b.rotation);
                }
            } else if (!nearly_equal(pose.position, b.position) || !nearly_equal(pose.rotation, b.rotation)) {
                bi.SetPositionAndRotation(b.id, to_jolt_r(pose.position), to_jolt(pose.rotation),
                                          b.motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate
                                                                               : JPH::EActivation::Activate);
            }
            b.position = pose.position;
            b.rotation = pose.rotation;
        });
        // Entities that lost their collider or were destroyed.
        for (auto it = bodies_.begin(); it != bodies_.end();) {
            if (it->second.seen != generation_) {
                destroy(it->second);
                it = bodies_.erase(it);
            } else {
                ++it;
            }
        }
        if (added > 64) system_->OptimizeBroadPhase();
    }

    void step(scene::Scene& scene, float dt, std::vector<ContactEvent>& events) override {
        sync(scene, dt);
        {
            VICTUS_PROFILE_SCOPE("physics.step3d");
            const auto t0 = std::chrono::steady_clock::now();
            const JPH::EPhysicsUpdateError err =
                system_->Update(dt, std::max(1, settings_.collision_steps), temp_.get(), jobs_.get());
            last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (err != JPH::EPhysicsUpdateError::None && !warned_)
                VICTUS_LOG_WARN("physics", "Jolt update reported error flags {} (raise max_body_pairs/max_contact_constraints)",
                                static_cast<unsigned>(err)),
                    warned_ = true;
            ++steps_;
        }
        VICTUS_PROFILE_SCOPE("physics.writeback3d");
        auto& bi = system_->GetBodyInterface();
        for (auto& [key, b] : bodies_) {
            if (b.motion != JPH::EMotionType::Dynamic || !bi.IsActive(b.id)) continue;
            const Entity e{static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32)};
            if (!scene.world().alive(e)) continue;
            JPH::RVec3 p;
            JPH::Quat q;
            bi.GetPositionAndRotation(b.id, p, q);
            b.position = {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()), static_cast<float>(p.GetZ())};
            b.rotation = from_jolt(q);
            write_world_pose(scene, e, b.position, b.rotation);
        }
        const std::size_t before = events.size();
        contacts_->drain(events);
        contacts_last_ = static_cast<std::uint32_t>(events.size() - before);
    }

    void clear() override {
        for (auto& [k, b] : bodies_) destroy(b);
        bodies_.clear();
        if (contacts_) contacts_->reset();
    }

    bool has_body(Entity e) const override { return bodies_.contains(e.bits()); }

    std::optional<RaycastHit> raycast(math::Vec3 o, math::Vec3 d, float max, std::uint32_t mask,
                                      Entity ignore) const override {
        const float len = math::length(d);
        if (len <= 1e-8f || max <= 0) return std::nullopt;
        d = d * (1.0f / len);
        const JPH::RRayCast ray{to_jolt_r(o), to_jolt(d * max)};
        JPH::RayCastResult hit;
        const QueryLayerFilter lf(mask);
        const QueryBodyFilter bf(body_of(ignore));
        if (!system_->GetNarrowPhaseQuery().CastRay(ray, hit, {}, lf, bf)) return std::nullopt;
        RaycastHit out;
        const JPH::RVec3 p = ray.GetPointOnRay(hit.mFraction);
        out.point = {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()), static_cast<float>(p.GetZ())};
        out.distance = hit.mFraction * max;
        JPH::BodyLockRead lock(system_->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            out.entity = decode(body.GetUserData());
            out.normal = from_jolt(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p));
        }
        return out;
    }

    std::vector<Entity> overlap_sphere(math::Vec3 c, float r, std::uint32_t mask) const override {
        if (r <= 0) return {};
        JPH::SphereShape sphere(r);
        sphere.SetEmbedded();
        return overlap(sphere, JPH::RMat44::sTranslation(to_jolt_r(c)), mask);
    }

    std::vector<Entity> overlap_box(math::Vec3 c, math::Vec3 h, math::Quat q, std::uint32_t mask) const override {
        h = abs(h);
        if (h.x <= 0 || h.y <= 0 || h.z <= 0) return {};
        JPH::BoxShape box(to_jolt(h), std::min({0.05f, h.x, h.y, h.z}));
        box.SetEmbedded();
        return overlap(box, JPH::RMat44::sRotationTranslation(to_jolt(q), to_jolt_r(c)), mask);
    }

    math::Vec3 linear_velocity(Entity e) const override {
        const JPH::BodyID id = body_of(e);
        return id.IsInvalid() ? math::Vec3{} : from_jolt(system_->GetBodyInterface().GetLinearVelocity(id));
    }
    void set_linear_velocity(Entity e, math::Vec3 v) override {
        if (const JPH::BodyID id = dynamic_body(e); !id.IsInvalid())
            system_->GetBodyInterface().SetLinearVelocity(id, to_jolt(v));
    }
    math::Vec3 angular_velocity(Entity e) const override {
        const JPH::BodyID id = body_of(e);
        return id.IsInvalid() ? math::Vec3{} : from_jolt(system_->GetBodyInterface().GetAngularVelocity(id));
    }
    void set_angular_velocity(Entity e, math::Vec3 v) override {
        if (const JPH::BodyID id = dynamic_body(e); !id.IsInvalid())
            system_->GetBodyInterface().SetAngularVelocity(id, to_jolt(v));
    }
    void add_force(Entity e, math::Vec3 f) override {
        if (const JPH::BodyID id = dynamic_body(e); !id.IsInvalid()) system_->GetBodyInterface().AddForce(id, to_jolt(f));
    }
    void add_impulse(Entity e, math::Vec3 i) override {
        if (const JPH::BodyID id = dynamic_body(e); !id.IsInvalid())
            system_->GetBodyInterface().AddImpulse(id, to_jolt(i));
    }
    void add_torque(Entity e, math::Vec3 t) override {
        if (const JPH::BodyID id = dynamic_body(e); !id.IsInvalid())
            system_->GetBodyInterface().AddTorque(id, to_jolt(t));
    }
    void add_angular_impulse(Entity e, math::Vec3 i) override {
        if (const JPH::BodyID id = dynamic_body(e); !id.IsInvalid())
            system_->GetBodyInterface().AddAngularImpulse(id, to_jolt(i));
    }
    void set_gravity(math::Vec3 g) override { system_->SetGravity(to_jolt(g)); }
    math::Vec3 gravity() const override { return from_jolt(system_->GetGravity()); }
    void stats(PhysicsStats& out) const override {
        out.bodies_3d = system_->GetNumBodies();
        out.active_3d = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
        out.steps += steps_;
        out.contacts_last_step += contacts_last_;
        out.last_step_ms += last_ms_;
    }

private:
    struct Entry {
        JPH::BodyID id;
        JPH::EMotionType motion{JPH::EMotionType::Static};
        ColliderComponent collider;
        RigidBodyComponent rb;
        bool has_rb{false};
        math::Vec3 scale{1, 1, 1};
        math::Vec3 position;
        math::Quat rotation;
        std::uint64_t seen{0};
        bool moving{false};
    };

    JPH::BodyID body_of(Entity e) const {
        const auto it = bodies_.find(e.bits());
        return it == bodies_.end() ? JPH::BodyID() : it->second.id;
    }
    JPH::BodyID dynamic_body(Entity e) const {
        const auto it = bodies_.find(e.bits());
        return it == bodies_.end() || it->second.motion != JPH::EMotionType::Dynamic ? JPH::BodyID() : it->second.id;
    }

    static JPH::ShapeRefC make_shape(const ColliderComponent& c, math::Vec3 s) {
        JPH::ShapeRefC shape;
        const float rs = std::max(s.x, s.z);
        switch (c.shape) {
        case ColliderComponent::Shape::Box: {
            const math::Vec3 h{std::max(1e-3f, c.size.x * s.x * 0.5f), std::max(1e-3f, c.size.y * s.y * 0.5f),
                               std::max(1e-3f, c.size.z * s.z * 0.5f)};
            const float cr = std::min({0.05f, h.x * 0.5f, h.y * 0.5f, h.z * 0.5f});
            shape = new JPH::BoxShape(to_jolt(h), cr);
            break;
        }
        case ColliderComponent::Shape::Sphere:
            shape = new JPH::SphereShape(std::max(1e-3f, c.radius * std::max({s.x, s.y, s.z})));
            break;
        case ColliderComponent::Shape::Capsule: {
            const float r = std::max(1e-3f, c.radius * rs);
            const float half = std::max(0.0f, c.height * s.y * 0.5f - r);
            if (half <= 1e-4f) shape = new JPH::SphereShape(r);
            else shape = new JPH::CapsuleShape(half, r);
            break;
        }
        case ColliderComponent::Shape::Cylinder: {
            const float r = std::max(1e-3f, c.radius * rs);
            const float half = std::max(1e-3f, c.height * s.y * 0.5f);
            shape = new JPH::CylinderShape(half, r, std::min({0.05f, half * 0.5f, r * 0.5f}));
            break;
        }
        }
        const math::Vec3 off{c.offset.x * s.x, c.offset.y * s.y, c.offset.z * s.z};
        if (math::length(off) > 1e-6f) {
            JPH::RotatedTranslatedShapeSettings rts(to_jolt(off), JPH::Quat::sIdentity(), shape);
            auto res = rts.Create();
            if (res.IsValid()) shape = res.Get();
        }
        return shape;
    }

    bool create(Entity e, const ColliderComponent& col, const RigidBodyComponent* rb, const Pose& pose, math::Vec3 scale,
                Entry& out) {
        JPH::ShapeRefC shape = make_shape(col, scale);
        if (!shape) return false;
        JPH::EMotionType motion = JPH::EMotionType::Static;
        if (rb && rb->type == BodyType::Dynamic) motion = JPH::EMotionType::Dynamic;
        else if (rb && rb->type == BodyType::Kinematic) motion = JPH::EMotionType::Kinematic;
        // Triggers without a dynamic body are kinematic so they can detect static/kinematic bodies.
        if (col.is_trigger && motion == JPH::EMotionType::Static) motion = JPH::EMotionType::Kinematic;
        const bool moving = motion != JPH::EMotionType::Static;
        JPH::BodyCreationSettings bcs(shape, to_jolt_r(pose.position), to_jolt(pose.rotation), motion,
                                      make_layer(std::min<std::uint32_t>(col.layer, 15u), col.mask, moving));
        bcs.mUserData = encode(e);
        bcs.mFriction = std::max(0.0f, col.friction);
        bcs.mRestitution = std::clamp(col.restitution, 0.0f, 1.0f);
        bcs.mIsSensor = col.is_trigger;
        if (col.is_trigger) bcs.mCollideKinematicVsNonDynamic = true;
        if (rb) {
            bcs.mLinearDamping = std::max(0.0f, rb->linear_damping);
            bcs.mAngularDamping = std::max(0.0f, rb->angular_damping);
            bcs.mGravityFactor = rb->gravity_scale;
            bcs.mMotionQuality = rb->continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            if (motion == JPH::EMotionType::Dynamic) {
                bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bcs.mMassPropertiesOverride.mMass = std::max(1e-4f, rb->mass);
                bcs.mLinearVelocity = to_jolt(rb->linear_velocity);
                bcs.mAngularVelocity = to_jolt(rb->angular_velocity);
                if (rb->lock_rotation) bcs.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
            }
            if (rb->type == BodyType::Kinematic) bcs.mCollideKinematicVsNonDynamic = col.is_trigger;
        }
        const bool awake = moving && (!rb || rb->start_awake);
        out.id = system_->GetBodyInterface().CreateAndAddBody(bcs, awake ? JPH::EActivation::Activate
                                                                          : JPH::EActivation::DontActivate);
        if (out.id.IsInvalid()) {
            if (!warned_) VICTUS_LOG_WARN("physics", "body limit reached ({} bodies)", settings_.max_bodies);
            warned_ = true;
            return false;
        }
        out.motion = motion;
        out.collider = col;
        out.has_rb = rb != nullptr;
        if (rb) out.rb = *rb;
        out.scale = scale;
        out.position = pose.position;
        out.rotation = pose.rotation;
        return true;
    }

    void destroy(Entry& b) {
        if (b.id.IsInvalid()) return;
        auto& bi = system_->GetBodyInterface();
        bi.RemoveBody(b.id);
        bi.DestroyBody(b.id);
        b.id = {};
    }

    std::vector<Entity> overlap(const JPH::Shape& shape, JPH::RMat44Arg xf, std::uint32_t mask) const {
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
        JPH::CollideShapeSettings cs;
        const QueryLayerFilter lf(mask);
        const QueryBodyFilter bf{JPH::BodyID()};
        system_->GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sOne(), xf, cs, JPH::RVec3::sZero(), hits, {}, lf,
                                                    bf);
        std::vector<Entity> out;
        std::unordered_set<std::uint32_t> seen;
        const auto& bi = system_->GetBodyInterface();
        for (const auto& h : hits.mHits) {
            if (!seen.insert(h.mBodyID2.GetIndexAndSequenceNumber()).second) continue;
            const Entity e = decode(bi.GetUserData(h.mBodyID2));
            if (!e.is_null()) out.push_back(e);
        }
        std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.bits() < b.bits(); });
        return out;
    }

    PhysicsSettings settings_;
    BroadPhaseLayers bp_layers_;
    ObjectVsBroadPhase obj_vs_bp_;
    LayerPairs pairs_;
    std::unique_ptr<JPH::TempAllocatorImpl> temp_;
    std::unique_ptr<JPH::JobSystem> jobs_;
    std::unique_ptr<JPH::PhysicsSystem> system_;
    std::unique_ptr<Contacts> contacts_;
    std::unordered_map<std::uint64_t, Entry> bodies_;
    std::uint64_t generation_{0};
    std::uint64_t steps_{0};
    std::uint32_t contacts_last_{0};
    double last_ms_{0};
    bool warned_{false};
};

} // namespace

std::unique_ptr<World3D> World3D::create(const PhysicsSettings& settings) {
    return std::make_unique<JoltWorld>(settings);
}

} // namespace victus::physics::detail
