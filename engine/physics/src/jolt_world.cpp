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
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
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

// Disables collision between pairs of bodies joined by a Joint with collide_connected = false.
// Every VICTUS body gets a unique group id; the pair set only changes in sync(), never while the
// solver runs, so the lookup needs no lock.
class JointPairFilter final : public JPH::GroupFilter {
public:
    static std::uint64_t key(JPH::CollisionGroup::GroupID a, JPH::CollisionGroup::GroupID b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | b;
    }
    bool CanCollide(const JPH::CollisionGroup& a, const JPH::CollisionGroup& b) const override {
        if (pairs_.empty()) return true;
        const auto it = pairs_.find(key(a.GetGroupID(), b.GetGroupID()));
        return it == pairs_.end() || it->second == 0;
    }
    void add(std::uint64_t k) { ++pairs_[k]; }
    void remove(std::uint64_t k) {
        if (auto it = pairs_.find(k); it != pairs_.end() && --it->second == 0) pairs_.erase(it);
    }
    void clear() { pairs_.clear(); }

private:
    std::unordered_map<std::uint64_t, int> pairs_;
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
    JoltWorld(const PhysicsSettings& s, CollisionMeshCache* meshes) : settings_(s), pairs_(s.layer_matrix), meshes_(meshes) {
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
        group_filter_ = new JointPairFilter();
    }
    ~JoltWorld() override {
        wait_for_shape_builds();
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
            if (scene.world().get<CharacterControllerComponent>(e)) return; // the character owns the shape
            const RigidBodyComponent* rb = scene.world().get<RigidBodyComponent>(e);
            const Pose pose = world_pose(scene, e);
            // Mesh and convex shapes keep the sign so mirrored instances collide mirrored.
            const math::Vec3 scale = uses_mesh(col) ? pose.scale : abs(pose.scale);
            std::shared_ptr<const CollisionMesh> mesh;
            if (uses_mesh(col)) {
                mesh = meshes_ ? meshes_->resolve(scene, e, col) : nullptr;
                if (!mesh) {
                    if (warned_mesh_.insert(e.bits()).second)
                        VICTUS_LOG_WARN("physics", "entity {}: {} collider has no geometry (set mesh_asset, add a MeshRenderer "
                                                   "with a model or primitive, or a procedural mesh); no body created",
                                        e.index, col.shape == ColliderComponent::Shape::Mesh ? "mesh" : "convex");
                    return; // a body built before (geometry removed) is destroyed below as unseen
                }
            }
            auto it = bodies_.find(e.bits());
            if (it != bodies_.end()) {
                Entry& b = it->second;
                const bool same = b.collider == col && b.has_rb == (rb != nullptr) && (!rb || b.rb == *rb) &&
                                  nearly_equal(b.scale, scale, 1e-4f) && b.mesh == mesh;
                if (!same) {
                    destroy(b);
                    bodies_.erase(it);
                    it = bodies_.end();
                }
            }
            if (it == bodies_.end()) {
                Entry b;
                b.mesh = mesh;
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
        if (shape_cache_.size() > 64) prune_shape_cache();
        sync_heightfields(scene);
        if (added > 64) system_->OptimizeBroadPhase();
        sync_characters(scene);
        sync_joints(scene);
    }

    void step(scene::Scene& scene, float dt, std::vector<ContactEvent>& events) override {
        sync(scene, dt);
        update_characters(dt);
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
        for (auto& [key, c] : chars_) {
            const Entity e{static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32)};
            if (!scene.world().alive(e)) continue;
            c.position = from_jolt(c.ch->GetPosition());
            write_world_pose(scene, e, c.position, world_pose(scene, e).rotation);
        }
        check_joints(dt);
        const std::size_t before = events.size();
        contacts_->drain(events);
        contacts_last_ = static_cast<std::uint32_t>(events.size() - before);
    }

    void clear() override {
        for (auto& [k, j] : joints_) destroy_joint(j);
        joints_.clear();
        group_filter_->clear();
        for (auto& [k, c] : chars_) destroy_character(c);
        chars_.clear();
        for (auto& [k, b] : bodies_) destroy(b);
        bodies_.clear();
        for (auto& [k, f] : fields_) destroy_field(f);
        fields_.clear();
        if (contacts_) contacts_->reset();
        shape_cache_.clear();
        warned_mesh_.clear();
    }

    void invalidate_meshes() override {
        // Bodies compare their geometry pointer every sync: a reloaded mesh is a new pointer and
        // rebuilds them. Built shapes of geometry nobody holds any more are released here.
        prune_shape_cache();
        warned_mesh_.clear();
    }

    bool has_body(Entity e) const override { return bodies_.contains(e.bits()) || fields_.contains(e.bits()) || chars_.contains(e.bits()); }

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
        if (const auto c = chars_.find(e.bits()); c != chars_.end()) return from_jolt(c->second.ch->GetLinearVelocity());
        const JPH::BodyID id = body_of(e);
        return id.IsInvalid() ? math::Vec3{} : from_jolt(system_->GetBodyInterface().GetLinearVelocity(id));
    }
    void set_linear_velocity(Entity e, math::Vec3 v) override {
        if (auto c = chars_.find(e.bits()); c != chars_.end()) return c->second.ch->SetLinearVelocity(to_jolt(v));
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
        if (auto c = chars_.find(e.bits()); c != chars_.end()) {
            JPH::CharacterVirtual& ch = *c->second.ch;
            ch.SetLinearVelocity(ch.GetLinearVelocity() + to_jolt(i) / std::max(0.1f, c->second.comp.mass));
            return;
        }
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
    void move_character(Entity e, math::Vec3 v) override {
        if (auto c = chars_.find(e.bits()); c != chars_.end()) c->second.desired = {v.x, 0, v.z};
    }
    bool jump_character(Entity e, float speed) override {
        auto c = chars_.find(e.bits());
        if (c == chars_.end() || c->second.ch->GetGroundState() != JPH::CharacterBase::EGroundState::OnGround) return false;
        c->second.jump = std::max(0.0f, speed);
        return true;
    }
    CharacterState character_state(Entity e) const override {
        CharacterState s;
        const auto c = chars_.find(e.bits());
        if (c == chars_.end()) return s;
        const JPH::CharacterVirtual& ch = *c->second.ch;
        s.valid = true;
        s.grounded = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        s.on_steep_slope = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnSteepGround;
        if (ch.IsSupported()) {
            s.ground_normal = from_jolt(ch.GetGroundNormal());
            s.ground = decode(ch.GetGroundUserData());
        }
        s.velocity = from_jolt(ch.GetLinearVelocity());
        return s;
    }
    JointState joint_state(Entity e) const override {
        JointState s;
        const auto it = joints_.find(e.bits());
        if (it == joints_.end()) return s;
        const Joint& j = it->second;
        s.valid = true;
        s.broken = j.broken;
        s.active = j.c != nullptr && !j.broken;
        s.force = j.force;
        if (!j.c) return s;
        switch (j.comp.type) {
        case JointComponent::Type::Hinge:
            s.angle = math::degrees(static_cast<const JPH::HingeConstraint*>(j.c.GetPtr())->GetCurrentAngle());
            break;
        case JointComponent::Type::Slider:
            s.position = static_cast<const JPH::SliderConstraint*>(j.c.GetPtr())->GetCurrentPosition();
            break;
        case JointComponent::Type::Distance: {
            const JPH::RVec3 p1 = j.c->GetBody1()->GetWorldTransform() * j.local1;
            const JPH::RVec3 p2 = j.c->GetBody2()->GetWorldTransform() * j.local2;
            s.position = static_cast<float>((p2 - p1).Length());
            break;
        }
        default: break;
        }
        return s;
    }
    void set_joint_motor(Entity e, bool enabled, float speed) override {
        auto it = joints_.find(e.bits());
        if (it == joints_.end()) return;
        it->second.comp.motor = enabled;
        it->second.comp.motor_speed = speed;
        apply_motor(it->second);
    }

    void set_gravity(math::Vec3 g) override { system_->SetGravity(to_jolt(g)); }
    math::Vec3 gravity() const override { return from_jolt(system_->GetGravity()); }
    void stats(PhysicsStats& out) const override {
        out.bodies_3d = system_->GetNumBodies();
        out.active_3d = system_->GetNumActiveBodies(JPH::EBodyType::RigidBody);
        out.steps += steps_;
        out.contacts_last_step += contacts_last_;
        out.last_step_ms += last_ms_;
        out.heightfield_builds += field_builds_;
        out.heightfield_updates += field_updates_;
        out.mesh_shapes_built += mesh_shapes_built_;
        for (const auto& kv : shape_cache_) out.mesh_shapes_pending += (kv.second.convex_build != nullptr) + (kv.second.triangles_build != nullptr);
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
        JPH::CollisionGroup::GroupID group{JPH::CollisionGroup::cInvalidGroup};
        std::shared_ptr<const CollisionMesh> mesh; // convex/mesh colliders
    };

    // A hull or triangle mesh being built on the job system; the job owns a reference.
    struct ShapeBuild {
        jobs::Counter counter;
        JPH::Shape::ShapeResult result;
    };
    // Unscaled Jolt shapes built from one CollisionMesh, shared by every body using it.
    struct MeshShapes {
        std::weak_ptr<const CollisionMesh> source;
        JPH::ShapeRefC convex, triangles;
        bool convex_failed{false}, triangles_failed{false};
        std::shared_ptr<ShapeBuild> convex_build, triangles_build; // in flight
    };

    struct Character {
        JPH::Ref<JPH::CharacterVirtual> ch;
        CharacterControllerComponent comp;
        math::Vec3 position;    // feet, world space, as last written to the Transform
        JPH::Vec3 desired{JPH::Vec3::sZero()};
        float jump{0};
        std::uint64_t seen{0};
    };

    struct Joint {
        JPH::Ref<JPH::TwoBodyConstraint> c;
        JointComponent comp;
        JPH::BodyID a, b;       // b invalid = world
        std::uint64_t pair{0};  // JointPairFilter key, 0 = none
        JPH::Vec3 local1{JPH::Vec3::sZero()}, local2{JPH::Vec3::sZero()}; // distance anchors in body space
        bool broken{false};
        float force{0};
        std::uint64_t seen{0};
    };

    JPH::BodyID body_of(Entity e) const {
        if (const auto c = chars_.find(e.bits()); c != chars_.end()) return c->second.ch->GetInnerBodyID();
        const auto it = bodies_.find(e.bits());
        return it == bodies_.end() ? JPH::BodyID() : it->second.id;
    }
    JPH::BodyID dynamic_body(Entity e) const {
        const auto it = bodies_.find(e.bits());
        return it == bodies_.end() || it->second.motion != JPH::EMotionType::Dynamic ? JPH::BodyID() : it->second.id;
    }

    static bool uses_mesh(const ColliderComponent& c) {
        return c.shape == ColliderComponent::Shape::Convex || c.shape == ColliderComponent::Shape::Mesh;
    }

    void prune_shape_cache() {
        // Entries with a build in flight stay until it finishes (the job holds the mesh alive).
        std::erase_if(shape_cache_, [](const auto& kv) {
            return kv.second.source.expired() && !kv.second.convex_build && !kv.second.triangles_build;
        });
    }

    // Every build ever submitted and not yet seen finished, including ones whose cache entry was
    // replaced or cleared: Jolt must stay initialised until they return.
    void wait_for_shape_builds() {
        if (!settings_.jobs) return;
        for (const auto& b : in_flight_) settings_.jobs->wait(b->counter);
        in_flight_.clear();
    }

    // Builds a convex hull (`triangles` false) or triangle mesh; thread-safe (reads only `mesh`).
    static JPH::Shape::ShapeResult build_mesh_shape(const CollisionMesh& mesh, bool triangles) {
        JPH::Shape::ShapeResult res;
        if (triangles) {
            JPH::VertexList verts;
            verts.reserve(mesh.positions.size());
            for (const math::Vec3& p : mesh.positions) verts.push_back(JPH::Float3(p.x, p.y, p.z));
            JPH::IndexedTriangleList tris;
            tris.reserve(mesh.indices.size() / 3);
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                tris.push_back(JPH::IndexedTriangle(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]));
            JPH::MeshShapeSettings ms(std::move(verts), std::move(tris));
            res = ms.Create();
        } else {
            // Duplicate positions (UV/normal seams) make the hull builder slower without changing it.
            std::vector<math::Vec3> pts(mesh.positions);
            std::sort(pts.begin(), pts.end(), [](math::Vec3 a, math::Vec3 b) {
                return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
            });
            pts.erase(std::unique(pts.begin(), pts.end(), [](math::Vec3 a, math::Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }),
                      pts.end());
            JPH::Array<JPH::Vec3> jp;
            jp.reserve(pts.size());
            for (const math::Vec3& p : pts) jp.push_back(to_jolt(p));
            JPH::ConvexHullShapeSettings hs(jp);
            res = hs.Create();
        }
        return res;
    }

    // Convex hull (`triangles` false) or triangle mesh of `mesh`, built once and cached. Large
    // meshes build on the job system: null is returned (no body yet) until a later sync finds the
    // build finished, so the body appears without stalling the frame that first saw the mesh.
    JPH::ShapeRefC mesh_shape(const std::shared_ptr<const CollisionMesh>& mesh, bool triangles) {
        MeshShapes& m = shape_cache_[mesh.get()];
        if (m.source.lock() != mesh) {
            // Address reused by a new mesh: an old in-flight build keeps its own mesh alive and
            // finishes into a result nobody reads.
            m = MeshShapes{mesh, {}, {}, false, false, {}, {}};
        }
        JPH::ShapeRefC& slot = triangles ? m.triangles : m.convex;
        bool& failed = triangles ? m.triangles_failed : m.convex_failed;
        std::shared_ptr<ShapeBuild>& build = triangles ? m.triangles_build : m.convex_build;
        if (slot || failed) return slot;
        JPH::Shape::ShapeResult res;
        if (build) {
            if (!build->counter.done()) return {};
            res = build->result;
            build.reset();
        } else if (settings_.jobs && settings_.async_mesh_triangles > 0 && mesh->indices.size() / 3 > settings_.async_mesh_triangles) {
            std::erase_if(in_flight_, [](const auto& b) { return b->counter.done(); });
            build = std::make_shared<ShapeBuild>();
            in_flight_.push_back(build);
            settings_.jobs->submit([b = build, mesh, triangles] { b->result = build_mesh_shape(*mesh, triangles); }, &build->counter,
                                   jobs::Priority::Background);
            return {};
        } else {
            res = build_mesh_shape(*mesh, triangles);
        }
        if (!res.IsValid()) {
            failed = true;
            VICTUS_LOG_WARN("physics", "cannot build a {} collision shape from {} vertices / {} triangles: {}",
                            triangles ? "mesh" : "convex", mesh->positions.size(), mesh->indices.size() / 3,
                            res.HasError() ? res.GetError().c_str() : "unknown error");
            return {};
        }
        slot = res.Get();
        ++mesh_shapes_built_;
        return slot;
    }

    JPH::ShapeRefC make_shape(const ColliderComponent& c, math::Vec3 s, const Entry& entry, bool dynamic) {
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
        case ColliderComponent::Shape::Convex:
        case ColliderComponent::Shape::Mesh: {
            if (!entry.mesh) return {};
            bool triangles = c.shape == ColliderComponent::Shape::Mesh;
            if (triangles && dynamic) {
                // Jolt triangle meshes have no volume (no mass properties) and do not collide
                // with other meshes: dynamic bodies use the hull instead.
                if (warned_dynamic_mesh_.insert(entry.mesh.get()).second)
                    VICTUS_LOG_WARN("physics", "a dynamic body uses a mesh collider; simulating its convex hull instead");
                triangles = false;
            }
            JPH::ShapeRefC base = mesh_shape(entry.mesh, triangles);
            if (!base) return {};
            shape = base;
            if (!nearly_equal(s, math::Vec3{1, 1, 1}, 1e-6f)) {
                // Keep mirroring (negative scale flips mesh winding in Jolt); avoid degenerate axes.
                const auto axis = [](float v) { return std::fabs(v) < 1e-4f ? std::copysign(1e-4f, v) : v; };
                const JPH::Vec3 js = base->MakeScaleValid(to_jolt(math::Vec3{axis(s.x), axis(s.y), axis(s.z)}));
                shape = new JPH::ScaledShape(base, js);
            }
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
        JPH::EMotionType motion = JPH::EMotionType::Static;
        if (rb && rb->type == BodyType::Dynamic) motion = JPH::EMotionType::Dynamic;
        else if (rb && rb->type == BodyType::Kinematic) motion = JPH::EMotionType::Kinematic;
        JPH::ShapeRefC shape = make_shape(col, scale, out, motion == JPH::EMotionType::Dynamic);
        if (!shape) return false;
        // Triggers without a dynamic body are kinematic so they can detect static/kinematic bodies.
        if (col.is_trigger && motion == JPH::EMotionType::Static) motion = JPH::EMotionType::Kinematic;
        const bool moving = motion != JPH::EMotionType::Static;
        JPH::BodyCreationSettings bcs(shape, to_jolt_r(pose.position), to_jolt(pose.rotation), motion,
                                      make_layer(std::min<std::uint32_t>(col.layer, 15u), col.mask, moving));
        bcs.mUserData = encode(e);
        out.group = next_group_++;
        bcs.mCollisionGroup = JPH::CollisionGroup(group_filter_, out.group, 0);
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
        // Constraints keep raw body pointers: drop the ones on this body before it is freed. sync_joints
        // rebuilds them against the replacement body.
        for (auto& [k, j] : joints_)
            if (j.c && (j.a == b.id || j.b == b.id)) {
                destroy_joint(j);
                j.a = {};
            }
        auto& bi = system_->GetBodyInterface();
        bi.RemoveBody(b.id);
        bi.DestroyBody(b.id);
        b.id = {};
    }

    // ---- height fields (terrain) ----

    struct Field {
        JPH::BodyID id;
        JPH::Ref<JPH::HeightFieldShape> shape;
        std::uint64_t revision{0};
        std::uint64_t height_revision{0};
        math::Vec3 position;
        math::Quat rotation;
        std::uint64_t seen{0};
    };

    void destroy_field(Field& f) {
        if (f.id.IsInvalid()) return;
        auto& bi = system_->GetBodyInterface();
        bi.RemoveBody(f.id);
        bi.DestroyBody(f.id);
        f.id = {};
    }

    bool create_field(Entity e, const StaticHeightfieldComponent& c, const Pose& pose, Field& out) {
        const std::uint32_t n = c.resolution;
        if (n < 2 || !c.heights || c.heights->size() != static_cast<std::size_t>(n) * n) return false;
        JPH::HeightFieldShapeSettings hs(c.heights->data(), JPH::Vec3(c.offset.x, c.offset.y, c.offset.z),
                                         JPH::Vec3(std::max(c.spacing.x, 1e-4f), 1.0f, std::max(c.spacing.y, 1e-4f)), n);
        hs.mBlockSize = n >= 1025 ? 4 : 2;
        // Quantize to about a millimetre per sample relative to each block's range.
        hs.mBitsPerSample = std::max<std::uint32_t>(8, hs.CalculateBitsPerSampleForError(0.001f));
        if (c.height_range.y > c.height_range.x) { // reserve the range later in-place edits need
            hs.mMinHeightValue = c.height_range.x;
            hs.mMaxHeightValue = c.height_range.y;
        }
        auto res = hs.Create();
        if (res.HasError()) {
            VICTUS_LOG_WARN("physics", "height field: {}", res.GetError().c_str());
            return false;
        }
        JPH::BodyCreationSettings bcs(res.Get(), to_jolt_r(pose.position), to_jolt(pose.rotation), JPH::EMotionType::Static,
                                      make_layer(std::min<std::uint32_t>(c.layer, 15u), c.mask, false));
        bcs.mUserData = encode(e);
        bcs.mFriction = std::max(0.0f, c.friction);
        bcs.mRestitution = std::clamp(c.restitution, 0.0f, 1.0f);
        out.id = system_->GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
        if (out.id.IsInvalid()) return false;
        out.shape = static_cast<JPH::HeightFieldShape*>(const_cast<JPH::Shape*>(res.Get().GetPtr()));
        out.revision = c.revision;
        out.height_revision = c.height_revision;
        ++field_builds_;
        out.position = pose.position;
        out.rotation = pose.rotation;
        return true;
    }

    // Rewrites the samples in `c.dirty` (rounded out to the shape's blocks) in place. False when
    // the rectangle is invalid or a height lies outside what the shape can encode: the caller
    // then rebuilds the shape.
    bool update_field(const StaticHeightfieldComponent& c, const Pose& pose, Field& f) {
        const std::uint32_t n = c.resolution;
        if (!f.shape || !c.heights || c.heights->size() != static_cast<std::size_t>(n) * n) return false;
        if (c.dirty.empty()) return true;
        if (c.dirty.x1 >= n || c.dirty.z1 >= n) return false;
        const float lo = f.shape->GetMinHeightValue(), hi = f.shape->GetMaxHeightValue();
        const std::uint32_t bs = f.shape->GetBlockSize(), count = f.shape->GetSampleCount();
        const std::uint32_t x0 = c.dirty.x0 / bs * bs, z0 = c.dirty.z0 / bs * bs;
        const std::uint32_t x1 = std::min(count, (c.dirty.x1 / bs + 1) * bs), z1 = std::min(count, (c.dirty.z1 / bs + 1) * bs);
        const std::uint32_t sx = x1 - x0, sz = z1 - z0;
        std::vector<float> block(static_cast<std::size_t>(sx) * sz, JPH::HeightFieldShapeConstants::cNoCollisionValue);
        float ymin = hi, ymax = lo;
        for (std::uint32_t z = z0; z < z1; ++z)
            for (std::uint32_t x = x0; x < x1; ++x) {
                if (x >= n || z >= n) continue; // padding beyond the field has no collision
                const float y = c.offset.y + (*c.heights)[static_cast<std::size_t>(z) * n + x];
                if (!(y >= lo && y <= hi)) return false;
                block[static_cast<std::size_t>(z - z0) * sx + (x - x0)] = y;
                ymin = std::min(ymin, y);
                ymax = std::max(ymax, y);
            }
        const JPH::Vec3 com = f.shape->GetCenterOfMass();
        f.shape->SetHeights(x0, z0, sx, sz, block.data(), static_cast<std::intptr_t>(sx), *temp_);
        auto& bi = system_->GetBodyInterface();
        bi.NotifyShapeChanged(f.id, com, false, JPH::EActivation::DontActivate);
        // Wake bodies resting on the edited area so they fall into a dug hole or ride up a bump.
        const math::Vec3 a{c.offset.x + static_cast<float>(x0) * c.spacing.x, ymin - 0.5f, c.offset.z + static_cast<float>(z0) * c.spacing.y};
        const math::Vec3 b{c.offset.x + static_cast<float>(x1) * c.spacing.x, ymax + 2.0f, c.offset.z + static_cast<float>(z1) * c.spacing.y};
        const JPH::AABox local(JPH::Vec3(a.x, a.y, a.z), JPH::Vec3(b.x, b.y, b.z));
        const JPH::Mat44 m = JPH::Mat44::sRotationTranslation(to_jolt(pose.rotation), JPH::Vec3(pose.position.x, pose.position.y, pose.position.z));
        bi.ActivateBodiesInAABox(local.Transformed(m), {}, {});
        ++field_updates_;
        return true;
    }

    void sync_heightfields(scene::Scene& scene) {
        auto& bi = system_->GetBodyInterface();
        scene.world().each<StaticHeightfieldComponent>([&](Entity e, StaticHeightfieldComponent& c) {
            const Pose pose = world_pose(scene, e);
            auto it = fields_.find(e.bits());
            if (it != fields_.end() && it->second.revision != c.revision) {
                destroy_field(it->second);
                fields_.erase(it);
                it = fields_.end();
            }
            if (it == fields_.end()) {
                Field f;
                if (!create_field(e, c, pose, f)) return;
                it = fields_.emplace(e.bits(), f).first;
            }
            if (it->second.height_revision != c.height_revision) {
                if (update_field(c, pose, it->second)) {
                    it->second.height_revision = c.height_revision;
                } else {
                    destroy_field(it->second);
                    fields_.erase(it);
                    Field f;
                    if (!create_field(e, c, pose, f)) return;
                    it = fields_.emplace(e.bits(), f).first;
                }
            }
            c.dirty = {};
            Field& f = it->second;
            f.seen = generation_;
            if (!nearly_equal(pose.position, f.position) || !nearly_equal(pose.rotation, f.rotation)) {
                bi.SetPositionAndRotation(f.id, to_jolt_r(pose.position), to_jolt(pose.rotation), JPH::EActivation::DontActivate);
                f.position = pose.position;
                f.rotation = pose.rotation;
            }
        });
        for (auto it = fields_.begin(); it != fields_.end();) {
            if (it->second.seen != generation_) {
                destroy_field(it->second);
                it = fields_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- characters ----

    void sync_characters(scene::Scene& scene) {
        scene.world().each<CharacterControllerComponent>([&](Entity e, CharacterControllerComponent& comp) {
            const Pose pose = world_pose(scene, e);
            auto it = chars_.find(e.bits());
            JPH::Vec3 velocity = JPH::Vec3::sZero();
            JPH::Vec3 desired = JPH::Vec3::sZero();
            if (it != chars_.end() && !(it->second.comp == comp)) {
                velocity = it->second.ch->GetLinearVelocity();
                desired = it->second.desired;
                destroy_character(it->second);
                chars_.erase(it);
                it = chars_.end();
            }
            if (it == chars_.end()) {
                Character c;
                create_character(e, comp, pose.position, c);
                c.ch->SetLinearVelocity(velocity);
                c.desired = desired;
                it = chars_.emplace(e.bits(), std::move(c)).first;
            }
            Character& c = it->second;
            c.seen = generation_;
            if (!nearly_equal(pose.position, c.position, 1e-4f)) { // gameplay teleported the character
                c.ch->SetPosition(to_jolt_r(pose.position));
                c.position = pose.position;
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

    void create_character(Entity e, const CharacterControllerComponent& comp, math::Vec3 feet, Character& out) {
        const float r = std::max(0.05f, comp.radius);
        const float half = std::max(0.01f, comp.height * 0.5f - r);
        // Origin at the feet: the capsule centre sits half + r above it.
        JPH::RotatedTranslatedShapeSettings rts(JPH::Vec3(0, half + r, 0), JPH::Quat::sIdentity(),
                                                new JPH::CapsuleShape(half, r));
        JPH::ShapeRefC shape = rts.Create().Get();
        JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
        cs->mShape = shape;
        cs->mInnerBodyShape = shape; // a kinematic twin so raycasts, triggers and dynamic bodies see it
        cs->mInnerBodyLayer = make_layer(std::min<std::uint32_t>(comp.layer, 15u), comp.mask, true);
        cs->mMaxSlopeAngle = JPH::DegreesToRadians(std::clamp(comp.max_slope, 1.0f, 89.0f));
        cs->mMass = std::max(0.1f, comp.mass);
        cs->mMaxStrength = std::max(0.0f, comp.push_force);
        // Contacts below the lower hemisphere centre support the character.
        cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -r);
        cs->mEnhancedInternalEdgeRemoval = true;
        out.ch = new JPH::CharacterVirtual(cs, to_jolt_r(feet), JPH::Quat::sIdentity(), encode(e), system_.get());
        out.comp = comp;
        out.position = feet;
        if (!out.ch->GetInnerBodyID().IsInvalid())
            system_->GetBodyInterface().SetUserData(out.ch->GetInnerBodyID(), encode(e));
        const JPH::ObjectLayer layer = cs->mInnerBodyLayer;
        const QueryBodyFilter bf(out.ch->GetInnerBodyID());
        out.ch->RefreshContacts(system_->GetDefaultBroadPhaseLayerFilter(layer), system_->GetDefaultLayerFilter(layer), bf,
                                {}, *temp_);
    }

    void destroy_character(Character& c) {
        c.ch = nullptr; // the destructor removes the inner body
    }

    void update_characters(float dt) {
        if (chars_.empty() || dt <= 0) return;
        VICTUS_PROFILE_SCOPE("physics.characters");
        for (auto& [key, c] : chars_) {
            JPH::CharacterVirtual& ch = *c.ch;
            const CharacterControllerComponent& comp = c.comp;
            const JPH::Vec3 gravity = system_->GetGravity() * comp.gravity_scale;
            ch.UpdateGroundVelocity();
            const JPH::Vec3 up = ch.GetUp();
            const JPH::Vec3 current = ch.GetLinearVelocity();
            const JPH::Vec3 vertical = up * up.Dot(current);
            const JPH::Vec3 horizontal = current - vertical;
            const JPH::Vec3 ground = ch.GetGroundVelocity();
            const bool on_ground = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            // Moving towards the ground (not launched off it this step) = keep the feet planted.
            const bool settled = (vertical - up * up.Dot(ground)).Dot(up) < 0.1f;
            const bool planted = on_ground && settled && c.jump <= 0;
            JPH::Vec3 v;
            if (on_ground && settled) {
                v = ground + c.desired; // ride moving platforms
                if (c.jump > 0) v += up * c.jump;
            } else {
                const float k = comp.air_control >= 1.0f ? 1.0f
                                                         : 1.0f - std::exp(-std::max(0.0f, comp.air_control) * 12.0f * dt);
                v = vertical + horizontal + (c.desired - horizontal) * k;
            }
            c.jump = 0;
            // On walkable ground only the part of gravity pressing into the surface applies, so characters
            // stand still on slopes up to max_slope instead of creeping downhill.
            if (planted) v += ch.GetGroundNormal() * ch.GetGroundNormal().Dot(gravity) * dt;
            else v += gravity * dt;
            ch.SetLinearVelocity(v);
            JPH::CharacterVirtual::ExtendedUpdateSettings us;
            const float step = std::max(0.0f, comp.step_height);
            us.mStickToFloorStepDown = JPH::Vec3(0, -step, 0);
            us.mWalkStairsStepUp = JPH::Vec3(0, step, 0);
            const JPH::ObjectLayer layer = make_layer(std::min<std::uint32_t>(comp.layer, 15u), comp.mask, true);
            const QueryBodyFilter bf(ch.GetInnerBodyID());
            ch.ExtendedUpdate(dt, gravity, us, system_->GetDefaultBroadPhaseLayerFilter(layer),
                              system_->GetDefaultLayerFilter(layer), bf, {}, *temp_);
        }
    }

    // ---- joints ----

    void sync_joints(scene::Scene& scene) {
        scene.world().each<JointComponent>([&](Entity e, JointComponent& comp) {
            const auto ia = bodies_.find(e.bits());
            const Entry* other = nullptr;
            bool resolved = ia != bodies_.end();
            if (resolved && !comp.connected.is_nil()) {
                const Entity oe = scene.find(comp.connected);
                const auto ib = oe.is_null() ? bodies_.end() : bodies_.find(oe.bits());
                if (ib == bodies_.end() || oe == e) resolved = false;
                else other = &ib->second;
            }
            // A constraint needs at least one simulated body.
            if (resolved && ia->second.motion != JPH::EMotionType::Dynamic &&
                (!other || other->motion != JPH::EMotionType::Dynamic))
                resolved = false;
            auto it = joints_.find(e.bits());
            if (!resolved) {
                if (it != joints_.end()) {
                    destroy_joint(it->second);
                    joints_.erase(it);
                }
                return;
            }
            const JPH::BodyID a = ia->second.id, b = other ? other->id : JPH::BodyID();
            if (it != joints_.end()) {
                Joint& j = it->second;
                JointComponent x = j.comp;
                x.motor = comp.motor;
                x.motor_speed = comp.motor_speed;
                x.motor_force = comp.motor_force;
                if (!j.c && !j.broken) { // a body was rebuilt
                    joints_.erase(it);
                    it = joints_.end();
                } else if (j.a != a || j.b != b || !(x == comp)) {
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
                create_joint(scene, e, ia->second, other, j);
                it = joints_.emplace(e.bits(), std::move(j)).first;
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

    void create_joint(scene::Scene& scene, Entity e, const Entry& body, const Entry* other, Joint& j) {
        const JointComponent& c = j.comp;
        const math::Mat4 m = scene.world_matrix(e);
        const math::Vec3 anchor = m.transform_point(c.anchor);
        math::Vec3 axis = m.transform_point(c.axis) - m.transform_point({});
        axis = math::length(axis) > 1e-6f ? math::normalize(axis) : math::Vec3{0, 1, 0};
        // Any unit vector perpendicular to the axis.
        const math::Vec3 helper = std::abs(axis.y) < 0.9f ? math::Vec3{0, 1, 0} : math::Vec3{1, 0, 0};
        const math::Vec3 normal = math::normalize(math::cross(axis, helper));
        const JPH::RVec3 p = to_jolt_r(anchor);
        const JPH::Vec3 ax = to_jolt(axis), nx = to_jolt(normal);
        JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
        switch (c.type) {
        case JointComponent::Type::Fixed: {
            auto* s = new JPH::FixedConstraintSettings();
            s->mAutoDetectPoint = true;
            settings = s;
            break;
        }
        case JointComponent::Type::Point: {
            auto* s = new JPH::PointConstraintSettings();
            s->mPoint1 = s->mPoint2 = p;
            settings = s;
            break;
        }
        case JointComponent::Type::Hinge: {
            auto* s = new JPH::HingeConstraintSettings();
            s->mPoint1 = s->mPoint2 = p;
            s->mHingeAxis1 = s->mHingeAxis2 = ax;
            s->mNormalAxis1 = s->mNormalAxis2 = nx;
            if (c.limits) {
                s->mLimitsMin = JPH::DegreesToRadians(std::clamp(std::min(c.min_limit, c.max_limit), -180.0f, 0.0f));
                s->mLimitsMax = JPH::DegreesToRadians(std::clamp(std::max(c.min_limit, c.max_limit), 0.0f, 180.0f));
            }
            s->mMotorSettings.SetTorqueLimit(std::max(0.0f, c.motor_force));
            settings = s;
            break;
        }
        case JointComponent::Type::Slider: {
            auto* s = new JPH::SliderConstraintSettings();
            s->mPoint1 = s->mPoint2 = p;
            s->SetSliderAxis(ax);
            if (c.limits) {
                s->mLimitsMin = std::min(0.0f, std::min(c.min_limit, c.max_limit));
                s->mLimitsMax = std::max(0.0f, std::max(c.min_limit, c.max_limit));
            }
            s->mMotorSettings.SetForceLimit(std::max(0.0f, c.motor_force));
            settings = s;
            break;
        }
        case JointComponent::Type::Distance: {
            auto* s = new JPH::DistanceConstraintSettings();
            const math::Vec3 p1 = other ? scene.world_matrix(scene.find(c.connected)).transform_point(c.connected_anchor)
                                        : c.connected_anchor;
            s->mPoint1 = to_jolt_r(p1);
            s->mPoint2 = p;
            if (c.limits) {
                s->mMinDistance = std::max(0.0f, std::min(c.min_limit, c.max_limit));
                s->mMaxDistance = std::max(s->mMinDistance, std::max(c.min_limit, c.max_limit));
            }
            settings = s;
            break;
        }
        }
        auto& bi = system_->GetBodyInterface();
        // Body 1 is the connected body (or the world) and body 2 this entity, so slider positions and hinge
        // angles measure this entity's motion relative to what it is attached to.
        JPH::TwoBodyConstraint* constraint = bi.CreateConstraint(settings, other ? other->id : JPH::BodyID(), body.id);
        if (!constraint) return;
        j.c = constraint;
        system_->AddConstraint(constraint);
        if (c.type == JointComponent::Type::Distance) {
            const auto* s = static_cast<const JPH::DistanceConstraintSettings*>(settings.GetPtr());
            j.local1 = JPH::Vec3(constraint->GetBody1()->GetWorldTransform().Inversed() * s->mPoint1);
            j.local2 = JPH::Vec3(constraint->GetBody2()->GetWorldTransform().Inversed() * s->mPoint2);
        }
        if (other && !c.collide_connected) {
            j.pair = JointPairFilter::key(body.group, other->group);
            group_filter_->add(j.pair);
        }
        apply_motor(j);
        bi.ActivateConstraint(constraint);
    }

    void apply_motor(Joint& j) {
        if (!j.c || j.broken) return;
        const JointComponent& c = j.comp;
        const JPH::EMotorState state = c.motor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off;
        if (c.type == JointComponent::Type::Hinge) {
            auto* h = static_cast<JPH::HingeConstraint*>(j.c.GetPtr());
            h->GetMotorSettings().SetTorqueLimit(std::max(0.0f, c.motor_force));
            h->SetMotorState(state);
            h->SetTargetAngularVelocity(JPH::DegreesToRadians(c.motor_speed));
        } else if (c.type == JointComponent::Type::Slider) {
            auto* s = static_cast<JPH::SliderConstraint*>(j.c.GetPtr());
            s->GetMotorSettings().SetForceLimit(std::max(0.0f, c.motor_force));
            s->SetMotorState(state);
            s->SetTargetVelocity(c.motor_speed);
        } else {
            return;
        }
        system_->GetBodyInterface().ActivateConstraint(j.c);
    }

    void destroy_joint(Joint& j) {
        if (j.pair) group_filter_->remove(j.pair);
        j.pair = 0;
        if (j.c) system_->RemoveConstraint(j.c);
        j.c = nullptr;
    }

    // Reaction magnitude of the last step (N, or N*m for rotation-only reactions) and breaking.
    void check_joints(float dt) {
        if (joints_.empty() || dt <= 0) return;
        const auto len2 = [](const JPH::Vector<2>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1]); };
        for (auto& [key, j] : joints_) {
            if (!j.c || j.broken) continue;
            float linear = 0, angular = 0;
            switch (j.comp.type) {
            case JointComponent::Type::Fixed: {
                const auto* c = static_cast<const JPH::FixedConstraint*>(j.c.GetPtr());
                linear = c->GetTotalLambdaPosition().Length();
                angular = c->GetTotalLambdaRotation().Length();
                break;
            }
            case JointComponent::Type::Point:
                linear = static_cast<const JPH::PointConstraint*>(j.c.GetPtr())->GetTotalLambdaPosition().Length();
                break;
            case JointComponent::Type::Hinge: {
                const auto* c = static_cast<const JPH::HingeConstraint*>(j.c.GetPtr());
                linear = c->GetTotalLambdaPosition().Length();
                angular = len2(c->GetTotalLambdaRotation()) + std::abs(c->GetTotalLambdaRotationLimits());
                break;
            }
            case JointComponent::Type::Slider: {
                const auto* c = static_cast<const JPH::SliderConstraint*>(j.c.GetPtr());
                linear = len2(c->GetTotalLambdaPosition()) + std::abs(c->GetTotalLambdaPositionLimits());
                angular = c->GetTotalLambdaRotation().Length();
                break;
            }
            case JointComponent::Type::Distance:
                linear = std::abs(static_cast<const JPH::DistanceConstraint*>(j.c.GetPtr())->GetTotalLambdaPosition());
                break;
            }
            // Lambdas are impulses accumulated over the step.
            j.force = std::max(linear, angular) / dt;
            if (j.comp.break_force > 0 && j.force > j.comp.break_force) {
                j.c->SetEnabled(false);
                j.broken = true;
                if (j.pair) group_filter_->remove(j.pair); // broken pieces collide again
                j.pair = 0;
                system_->GetBodyInterface().ActivateConstraint(j.c);
            }
        }
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
    std::unordered_map<std::uint64_t, Field> fields_;
    std::uint64_t field_builds_{0}, field_updates_{0};
    std::unordered_map<std::uint64_t, Character> chars_;
    std::unordered_map<std::uint64_t, Joint> joints_;
    JPH::Ref<JointPairFilter> group_filter_;
    JPH::CollisionGroup::GroupID next_group_{0};
    std::uint64_t generation_{0};
    std::uint64_t steps_{0};
    std::uint32_t contacts_last_{0};
    double last_ms_{0};
    bool warned_{false};
    CollisionMeshCache* meshes_{nullptr};
    std::unordered_map<const CollisionMesh*, MeshShapes> shape_cache_;
    std::unordered_set<std::uint64_t> warned_mesh_;
    std::unordered_set<const CollisionMesh*> warned_dynamic_mesh_;
    std::uint64_t mesh_shapes_built_{0};
    std::vector<std::shared_ptr<ShapeBuild>> in_flight_; // background shape builds (see wait_for_shape_builds)
};

} // namespace

std::unique_ptr<World3D> World3D::create(const PhysicsSettings& settings, CollisionMeshCache* meshes) {
    return std::make_unique<JoltWorld>(settings, meshes);
}

} // namespace victus::physics::detail
