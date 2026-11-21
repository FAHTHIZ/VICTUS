// SPDX-License-Identifier: Apache-2.0
// Foot placement and foot locking (ADR-0163). See foot_placement.hpp for the model.
#include "victus/animation/foot_placement.hpp"
#include "victus/animation/ik.hpp"
#include "ik_internal.hpp"

#include "victus/scene/reflect.hpp"
#include "victus/scene/scene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace victus::animation {

using namespace math;
using scene::Entity;
using scene::Scene;

namespace {

constexpr Vec3 kUp{0, 1, 0};
constexpr float kEps = 1e-6f;

// Runtime (not serialized) per foot.
struct FootRuntime {
    bool active{false};     // evaluated this frame (finish_foot_placement only touches these)
    bool has_prev{false};
    Vec3 prev;              // animated world position last frame
    float rest_height{0};   // lowest animated height above the root seen (slowly relaxing)
    float offset{0};        // smoothed vertical correction (metres)
    Vec3 normal{kUp};       // smoothed ground normal
    bool grounded{false};
    Vec3 ground_point, ground_normal{kUp};
    Vec3 probe_from, probe_to;
    bool locked{false};
    Vec3 lock_point;
    float lock_weight{0};   // 1 while locked, fading after release
    Quat tilt{};            // applied after IK
    Entity root;
};

// Runtime per character root.
struct FootPlacementRuntime {
    float pelvis_offset{0};
    bool has_written{false};
    Entity pelvis;
    Vec3 pelvis_input, pelvis_written; // local positions
};

Quat scale_rotation(Quat q, float t) noexcept {
    if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
    const float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < kEps) return {};
    const float angle = 2.0f * std::atan2(s, q.w);
    return Quat::axis_angle({q.x / s, q.y / s, q.z / s}, angle * t);
}

Vec3 world_pos(Scene& scene, Entity e) { return scene.world_matrix(e).transform_point({}); }

Quat world_rotation(Scene& scene, Entity e) {
    if (e.is_null()) return {};
    Vec3 t, s;
    Quat r;
    decompose(scene.world_matrix(e), t, r, s);
    return normalize(r);
}

// The nearest ancestor (or `e` itself) with an enabled FootPlacement.
Entity placement_root(Scene& scene, Entity e) {
    for (Entity p = scene.parent(e); !p.is_null(); p = scene.parent(p))
        if (const auto* fp = scene.world().get<FootPlacementComponent>(p); fp && fp->enabled) return p;
    return {};
}

bool usable_ik(const IkConstraintComponent* ik) {
    return ik && ik->enabled && ik->solver == IkSolver::TwoBone && ik->target == Uuid{};
}

} // namespace

void register_foot_placement_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<FootPlacementComponent>("FootPlacement", "Animation")
            .field(&FootPlacementComponent::enabled, "enabled")
            .field(&FootPlacementComponent::pelvis, "pelvis",
                   {.tooltip = "Bone lowered so the lower foot reaches the ground (hips); empty = never move the body",
                    .hint = "entity"})
            .field(&FootPlacementComponent::probe_up, "probe_up", {.min = 0, .max = 5, .speed = 0.01f, .tooltip = "Highest step a foot climbs, metres"})
            .field(&FootPlacementComponent::probe_down, "probe_down", {.min = 0, .max = 5, .speed = 0.01f, .tooltip = "Deepest drop a foot reaches, metres"})
            .field(&FootPlacementComponent::max_pelvis_drop, "max_pelvis_drop", {.min = 0, .max = 2, .speed = 0.01f, .tooltip = "metres"})
            .field(&FootPlacementComponent::smoothing, "smoothing",
                   {.min = 0, .max = 100, .speed = 0.1f, .tooltip = "1/s: how fast corrections follow the ground (0 = instant)"})
            .field(&FootPlacementComponent::align_to_ground, "align_to_ground", {.tooltip = "Tilt feet to the ground normal"})
            .field(&FootPlacementComponent::max_slope, "max_slope", {.min = 0, .max = 89, .speed = 0.5f, .tooltip = "Largest foot tilt, degrees"})
            .field(&FootPlacementComponent::lock_feet, "lock_feet", {.tooltip = "Keep planted feet from sliding"})
            .field(&FootPlacementComponent::lock_speed, "lock_speed", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "m/s: a slower foot near the ground plants"})
            .field(&FootPlacementComponent::lock_height, "lock_height",
                   {.min = 0, .max = 1, .speed = 0.005f, .tooltip = "A foot this close above its rest height counts as on the ground"})
            .field(&FootPlacementComponent::release_distance, "release_distance",
                   {.min = 0, .max = 2, .speed = 0.01f, .tooltip = "A locked foot this far from the animated one lets go, metres"})
            .field(&FootPlacementComponent::mask, "mask", {.hint = "bitmask16"})
            .commit();
        scene::Schema<FootContactComponent>("FootContact", "Animation")
            .field(&FootContactComponent::enabled, "enabled")
            .field(&FootContactComponent::lock, "lock", {.tooltip = "May plant (foot locking)"})
            .commit();
    });
}

FootPlacementStats prepare_foot_placement(Scene& scene, const GroundProbe& probe, float dt) {
    FootPlacementStats stats;
    ecs::World& world = scene.world();
    // Feet grouped by their character root (ordered: deterministic).
    std::map<std::uint64_t, std::vector<Entity>> groups;
    world.each<FootContactComponent>([&](Entity e, FootContactComponent& c) {
        if (auto* rt = world.get<FootRuntime>(e)) rt->active = false;
        if (!c.enabled) return;
        const Entity root = placement_root(scene, e);
        if (root.is_null() || !usable_ik(world.get<IkConstraintComponent>(e))) {
            ++stats.invalid;
            return;
        }
        groups[root.bits()].push_back(e);
    });
    // Structural changes before any component reference is held.
    for (auto& [key, feet] : groups) {
        (void)key;
        const Entity root = placement_root(scene, feet.front());
        if (!world.has<FootPlacementRuntime>(root)) world.add<FootPlacementRuntime>(root);
        for (const Entity f : feet)
            if (!world.has<FootRuntime>(f)) world.add<FootRuntime>(f);
    }
    for (auto& [key, feet] : groups) {
        (void)key;
        const Entity root = placement_root(scene, feet.front());
        const FootPlacementComponent fp = *world.get<FootPlacementComponent>(root);
        ++stats.characters;
        auto& prt = *world.get<FootPlacementRuntime>(root);
        // Animated pose first: undo last frame's pelvis shift and IK output where nothing re-animated them.
        const Entity pelvis = fp.pelvis == Uuid{} ? Entity{} : scene.find(fp.pelvis);
        auto* pelvis_t = pelvis.is_null() ? nullptr : world.get<scene::Transform>(pelvis);
        if (pelvis_t && prt.has_written && prt.pelvis == pelvis && pelvis_t->position == prt.pelvis_written)
            pelvis_t->position = prt.pelvis_input;
        for (const Entity f : feet) detail::restore_ik_input(scene, f);

        const float k = dt > 0 && fp.smoothing > 0 ? 1.0f - std::exp(-fp.smoothing * dt) : 1.0f;
        const Vec3 root_pos = world_pos(scene, root);
        const float up_len = std::max(fp.probe_up, 0.0f), down_len = std::max(fp.probe_down, 0.0f);
        struct Plan {
            Entity foot;
            Vec3 animated;
        };
        std::vector<Plan> plans;
        float lowest = 0; // most negative smoothed correction
        for (const Entity f : feet) {
            FootRuntime& rt = *world.get<FootRuntime>(f);
            if (rt.root != root) rt = FootRuntime{}; // re-parented or new: start clean
            rt.root = root;
            rt.active = true;
            ++stats.feet;
            const Vec3 anim = world_pos(scene, f);
            const float height = anim.y - root_pos.y; // the animation's foot height above the ground plane
            rt.rest_height = rt.has_prev ? std::min(height, rt.rest_height + 0.05f * std::max(dt, 0.0f)) : height;
            rt.probe_from = anim + kUp * up_len;
            rt.probe_to = anim - kUp * down_len;
            // Without a probe (edit-mode previews) the targets track the animation, so IK leaves the legs alone.
            const auto hit = probe ? probe(rt.probe_from, kUp * -1.0f, up_len + down_len, fp.mask, root) : std::nullopt;
            rt.grounded = hit.has_value();
            float target_offset = 0;
            Vec3 target_normal = kUp;
            if (hit) {
                ++stats.grounded;
                rt.ground_point = hit->point;
                rt.ground_normal = length(hit->normal) > kEps ? normalize(hit->normal) : kUp;
                // The foot keeps its animated height above the ground it is over, not the root's plane.
                target_offset = std::clamp(hit->point.y - root_pos.y, -down_len, up_len);
                target_normal = rt.ground_normal.y > 0 ? rt.ground_normal : kUp;
            }
            rt.offset += (target_offset - rt.offset) * k;
            rt.normal = normalize(rt.normal + (target_normal - rt.normal) * k);
            if (length(rt.normal) < 0.5f) rt.normal = kUp;
            lowest = std::min(lowest, rt.offset);

            // Foot locking on the animated motion (world space, before any correction).
            const auto* contact = world.get<FootContactComponent>(f);
            const bool can_lock = fp.lock_feet && contact && contact->lock && rt.grounded;
            if (dt > 0 && rt.has_prev) {
                const Vec3 v = (anim - rt.prev) * (1.0f / dt);
                const float speed = length(v);
                const bool low = height - rt.rest_height < fp.lock_height;
                const Vec3 corrected = anim + kUp * rt.offset;
                // A released foot glides back to the animation before it may plant again (no pop).
                if (!rt.locked && can_lock && low && speed < fp.lock_speed && rt.lock_weight == 0) {
                    rt.locked = true;
                    rt.lock_point = corrected;
                } else if (rt.locked) {
                    const float dx = rt.lock_point.x - anim.x, dz = rt.lock_point.z - anim.z;
                    if (!can_lock || !low || std::sqrt(dx * dx + dz * dz) > fp.release_distance) rt.locked = false;
                }
            } else if (!can_lock) {
                rt.locked = false;
            }
            if (rt.locked) ++stats.locked;
            rt.lock_weight = rt.locked ? 1.0f : rt.lock_weight * (1.0f - k);
            if (rt.lock_weight < 1e-3f) rt.lock_weight = 0;
            rt.prev = anim;
            rt.has_prev = true;
            plans.push_back({f, anim});
        }

        // The pelvis drops by the largest downward correction so the lowest foot can reach.
        prt.pelvis_offset = std::clamp(lowest, -std::max(fp.max_pelvis_drop, 0.0f), 0.0f);
        prt.has_written = false;
        prt.pelvis = pelvis;
        if (pelvis_t && prt.pelvis_offset < 0) {
            const Entity parent = scene.parent(pelvis);
            const Mat4 parent_inv = parent.is_null() ? Mat4::identity() : inverse(scene.world_matrix(parent));
            prt.pelvis_input = pelvis_t->position;
            pelvis_t->position = pelvis_t->position + parent_inv.transform_vector(kUp * prt.pelvis_offset);
            prt.pelvis_written = pelvis_t->position;
            prt.has_written = true;
        }

        const float max_tilt = std::clamp(fp.max_slope, 0.0f, 89.0f) * std::numbers::pi_v<float> / 180.0f;
        for (const Plan& p : plans) {
            FootRuntime& rt = *world.get<FootRuntime>(p.foot);
            Vec3 target = p.animated + kUp * rt.offset;
            if (rt.lock_weight > 0) {
                target.x += (rt.lock_point.x - target.x) * rt.lock_weight;
                target.z += (rt.lock_point.z - target.z) * rt.lock_weight;
            }
            world.get<IkConstraintComponent>(p.foot)->target_position = target;
            rt.tilt = {};
            if (fp.align_to_ground) {
                Quat q = rotation_between(kUp, rt.normal);
                const float angle = std::acos(std::clamp(dot(kUp, rt.normal), -1.0f, 1.0f));
                if (angle > max_tilt && angle > kEps) q = scale_rotation(q, max_tilt / angle);
                rt.tilt = normalize(q);
            }
        }
    }
    return stats;
}

void finish_foot_placement(Scene& scene) {
    ecs::World& world = scene.world();
    world.each<FootRuntime>([&](Entity f, FootRuntime& rt) {
        if (!rt.active || rt.tilt == Quat{}) return;
        auto* t = world.get<scene::Transform>(f);
        const auto* ik = world.get<IkConstraintComponent>(f);
        if (!t || !ik) return;
        const float w = std::clamp(ik->weight, 0.0f, 1.0f);
        const Quat tilt = w >= 1.0f ? rt.tilt : scale_rotation(rt.tilt, w);
        const Quat parent = world_rotation(scene, scene.parent(f));
        const Quat now = normalize(parent * t->rotation);
        t->rotation = normalize(parent.conjugate() * normalize(tilt * now));
        detail::set_ik_output_rotation(scene, f, f, t->rotation);
    });
}

namespace {
std::string lower(std::string_view v) {
    std::string out(v);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
void descendants(const Scene& s, Entity e, std::vector<Entity>& out, int depth) {
    if (depth > 128 || out.size() >= 4096) return;
    for (const Entity c : s.children(e)) {
        out.push_back(c);
        descendants(s, c, out, depth + 1);
    }
}
// Joints between `e` and `root` (exclusive); 0 when `e` is not below `root`.
int depth_below(const Scene& s, Entity e, Entity root) {
    int d = 0;
    for (Entity p = s.parent(e); !p.is_null(); p = s.parent(p), ++d)
        if (p == root) return d;
    return 0;
}
} // namespace

std::size_t make_humanoid_foot_placement(Scene& scene, Entity root) {
    ecs::World& world = scene.world();
    if (!world.alive(root)) return 0;
    std::vector<Entity> all;
    descendants(scene, root, all, 0);
    Entity pelvis;
    std::vector<Entity> feet;
    for (const Entity e : all) {
        const auto* n = world.get<scene::Name>(e);
        if (!n) continue;
        const std::string name = lower(n->value);
        if (pelvis.is_null() && (name.find("hips") != std::string::npos || name.find("pelvis") != std::string::npos)) pelvis = e;
        const bool foot = name.find("foot") != std::string::npos || name.find("ankle") != std::string::npos;
        if (foot && name.find("toe") == std::string::npos && name.find("ball") == std::string::npos &&
            depth_below(scene, e, root) >= 2)
            feet.push_back(e);
    }
    // A foot inside another foot's subtree (Foot.L > FootEnd.L) is not a separate foot.
    std::erase_if(feet, [&](Entity f) {
        for (Entity p = scene.parent(f); !p.is_null() && p != root; p = scene.parent(p))
            if (std::find(feet.begin(), feet.end(), p) != feet.end()) return true;
        return false;
    });
    if (feet.empty()) return 0;
    for (const Entity f : feet) {
        if (!world.has<IkConstraintComponent>(f)) {
            IkConstraintComponent ik;
            ik.solver = IkSolver::TwoBone;
            ik.target_position = world_pos(scene, f);
            world.add<IkConstraintComponent>(f, ik);
        }
        if (!world.has<FootContactComponent>(f)) world.add<FootContactComponent>(f);
    }
    if (!world.has<FootPlacementComponent>(root)) world.add<FootPlacementComponent>(root);
    if (!pelvis.is_null()) world.get<FootPlacementComponent>(root)->pelvis = scene.uuid(pelvis);
    return feet.size();
}

std::size_t remove_foot_placement(Scene& scene, Entity root) {
    ecs::World& world = scene.world();
    if (!world.alive(root)) return 0;
    std::size_t removed = 0;
    std::vector<Entity> all;
    descendants(scene, root, all, 0);
    for (const Entity e : all) {
        if (!world.has<FootContactComponent>(e)) continue;
        world.remove<FootContactComponent>(e);
        ++removed;
        if (world.has<FootRuntime>(e)) world.remove<FootRuntime>(e);
        if (const auto* ik = world.get<IkConstraintComponent>(e); ik && ik->solver == IkSolver::TwoBone && ik->target == Uuid{}) {
            world.remove<IkConstraintComponent>(e);
            ++removed;
        }
    }
    if (world.has<FootPlacementComponent>(root)) {
        world.remove<FootPlacementComponent>(root);
        ++removed;
    }
    if (world.has<FootPlacementRuntime>(root)) world.remove<FootPlacementRuntime>(root);
    return removed;
}

void foot_placement_lines(Scene& scene, const std::function<void(Vec3, Vec3)>& emit) {
    scene.world().each<FootRuntime>([&](Entity, FootRuntime& rt) {
        if (!rt.active) return;
        emit(rt.probe_from, rt.probe_to);
        if (rt.grounded) {
            constexpr float s = 0.06f;
            const Vec3 g = rt.ground_point;
            emit(g - Vec3{s, 0, 0}, g + Vec3{s, 0, 0});
            emit(g - Vec3{0, 0, s}, g + Vec3{0, 0, s});
            emit(g, g + rt.ground_normal * 0.2f);
        }
        if (rt.locked) {
            constexpr float b = 0.08f;
            const Vec3 c = rt.lock_point;
            const Vec3 corners[4] = {{c.x - b, c.y, c.z - b}, {c.x + b, c.y, c.z - b}, {c.x + b, c.y, c.z + b}, {c.x - b, c.y, c.z + b}};
            for (int i = 0; i < 4; ++i) emit(corners[i], corners[(i + 1) % 4]);
        }
    });
}

} // namespace victus::animation
