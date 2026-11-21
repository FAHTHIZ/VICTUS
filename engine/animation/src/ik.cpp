// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/ik.hpp"
#include "ik_internal.hpp"

#include "victus/scene/reflect.hpp"
#include "victus/scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>

namespace victus::animation {

using namespace math;
using scene::Entity;
using scene::Scene;

namespace {
constexpr float kEps = 1e-6f;

Quat nlerp(Quat a, Quat b, float t) noexcept {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = {-b.x, -b.y, -b.z, -b.w};
    return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}

// Any unit vector perpendicular to v.
Vec3 perpendicular(Vec3 v) noexcept {
    const Vec3 axis = std::abs(v.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return normalize(cross(v, axis));
}

// Scales the rotation angle of a unit quaternion (shortest arc) by t.
Quat scale_rotation(Quat q, float t) noexcept {
    if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
    const float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < kEps) return {};
    const float angle = 2.0f * std::atan2(s, q.w);
    return Quat::axis_angle({q.x / s, q.y / s, q.z / s}, angle * t);
}
} // namespace

Quat rotation_between(Vec3 from, Vec3 to) noexcept {
    const float lf = length(from), lt = length(to);
    if (lf < kEps || lt < kEps) return {};
    const Vec3 a = from * (1.0f / lf), b = to * (1.0f / lt);
    const float d = dot(a, b);
    if (d < -1.0f + 1e-6f) {
        const Vec3 axis = perpendicular(a);
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3 c = cross(a, b);
    return normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

TwoBoneResult solve_two_bone(Vec3 root, Vec3 mid, Vec3 tip, Vec3 target, Vec3 bend_hint, float max_extension) noexcept {
    TwoBoneResult r{mid, tip, true};
    const float l1 = length(mid - root), l2 = length(tip - mid);
    if (l1 < kEps || l2 < kEps) {
        r.reached = false;
        return r;
    }
    const Vec3 to_target = target - root;
    float dist = length(to_target);
    const Vec3 dir = dist > kEps ? to_target * (1.0f / dist) : normalize(tip - root);
    const float reach = (l1 + l2) * std::clamp(max_extension, 0.01f, 1.0f);
    const float min_reach = std::abs(l1 - l2) + 1e-4f * (l1 + l2);
    if (dist > reach) {
        r.reached = dist <= l1 + l2 + 1e-4f; // within the true reach, only softened
        dist = reach;
    } else if (dist < min_reach) {
        r.reached = false;
        dist = min_reach;
    }
    // Bend plane: perpendicular to dir, toward the hint (pole or current middle joint).
    Vec3 up = bend_hint - root;
    up = up - dir * dot(up, dir);
    if (length(up) < kEps) {
        up = mid - root;
        up = up - dir * dot(up, dir);
    }
    up = length(up) < kEps ? perpendicular(dir) : normalize(up);
    const float cos_a = std::clamp((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    r.mid = root + (dir * cos_a + up * sin_a) * l1;
    r.tip = r.mid + normalize(root + dir * dist - r.mid) * l2;
    return r;
}

namespace {
// `d` (unit) turned toward `ref` (unit) until the angle between them is at most `max_angle`.
Vec3 clamp_direction(Vec3 d, Vec3 ref, float max_angle) noexcept {
    const float angle = std::acos(std::clamp(dot(d, ref), -1.0f, 1.0f));
    if (angle <= max_angle || angle < kEps) return d;
    return normalize(scale_rotation(rotation_between(ref, d), std::max(0.0f, max_angle) / angle).rotate(ref));
}
} // namespace

FabrikResult solve_fabrik(std::span<Vec3> p, Vec3 target, std::uint32_t max_iterations, float tolerance,
                          float max_bend) noexcept {
    FabrikResult out;
    const std::size_t n = p.size();
    if (n < 2) return out;
    std::vector<float> len(n - 1);
    float total = 0;
    for (std::size_t i = 0; i + 1 < n; ++i) total += len[i] = length(p[i + 1] - p[i]);
    const Vec3 root = p[0];
    const bool limited = std::isfinite(max_bend) && max_bend < std::numbers::pi_v<float> - 1e-4f;
    const Vec3 first_dir = len[0] > kEps ? (p[1] - p[0]) * (1.0f / len[0]) : Vec3{0, 1, 0};
    // Places joint i + 1 from joint i along `dir`, inside the cone around the previous bone.
    auto place = [&](std::size_t i, Vec3 dir) {
        if (limited) dir = clamp_direction(dir, i == 0 ? first_dir : normalize(p[i] - p[i - 1]), max_bend);
        p[i + 1] = p[i] + dir * len[i];
    };
    if (length(target - root) >= total) { // out of reach: straighten toward the target
        const Vec3 dir = length(target - root) > kEps ? normalize(target - root) : Vec3{0, 1, 0};
        for (std::size_t i = 0; i + 1 < n; ++i) place(i, dir);
        out.error = length(p[n - 1] - target);
        out.reached = out.error <= tolerance;
        return out;
    }
    auto toward = [](Vec3 from, Vec3 to, float l, Vec3 fallback) {
        const Vec3 d = to - from;
        const float dl = length(d);
        return from + (dl > kEps ? d * (1.0f / dl) : fallback) * l;
    };
    out.error = length(p[n - 1] - target);
    while (out.error > tolerance && out.iterations < max_iterations) {
        ++out.iterations;
        p[n - 1] = target; // backward: end on the target
        for (std::size_t i = n - 1; i-- > 0;) p[i] = toward(p[i + 1], p[i], len[i], Vec3{0, -1, 0});
        p[0] = root; // forward: root back in place (and the joint limits applied)
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Vec3 next = toward(p[i], p[i + 1], len[i], Vec3{0, 1, 0});
            place(i, len[i] > kEps ? (next - p[i]) * (1.0f / len[i]) : Vec3{0, 1, 0});
        }
        const float error = length(p[n - 1] - target);
        if (limited && std::abs(error - out.error) < tolerance * 0.01f) {
            out.error = error; // a limit holds the chain: further passes cannot get closer
            break;
        }
        out.error = error;
    }
    out.reached = out.error <= tolerance;
    return out;
}

void rotations_from_positions(std::span<const Vec3> before, std::span<const Vec3> after, std::span<Quat> rot) noexcept {
    const std::size_t n = std::min({before.size(), after.size(), rot.size()});
    Quat acc{}; // turn accumulated from the ancestors in the chain
    for (std::size_t i = 0; i < n; ++i) {
        if (i + 1 < n) {
            const Vec3 old_dir = acc.rotate(before[i + 1] - before[i]);
            const Quat delta = rotation_between(old_dir, after[i + 1] - after[i]);
            acc = normalize(delta * acc);
        }
        rot[i] = normalize(acc * rot[i]);
    }
}

Quat aim_rotation(Vec3 pivot, Vec3 aim, Vec3 target, float max_angle) noexcept {
    const Vec3 want = target - pivot;
    if (length(want) < kEps || length(aim) < kEps) return {};
    Quat q = rotation_between(aim, want);
    const float angle = std::acos(std::clamp(dot(normalize(aim), normalize(want)), -1.0f, 1.0f));
    if (angle > max_angle && angle > kEps) q = scale_rotation(q, std::max(0.0f, max_angle) / angle);
    return q;
}

// ---- component and scene application ---------------------------------------------------------

namespace {

using detail::IkRuntime;

struct Joint {
    Entity e;
    Vec3 pos;
    Quat rot;   // world
    Quat local; // current local rotation (animated input)
};

Quat parent_world_rotation(Scene& scene, Entity e) {
    const Entity p = scene.parent(e);
    if (p.is_null()) return {};
    Vec3 t, s;
    Quat r;
    decompose(scene.world_matrix(p), t, r, s);
    return r;
}

bool target_of(Scene& scene, const IkConstraintComponent& c, Vec3& pos, Quat* rot) {
    if (c.target == Uuid{}) {
        pos = c.target_position;
        return true;
    }
    const Entity t = scene.find(c.target);
    if (t.is_null()) return false;
    Vec3 s;
    Quat r;
    decompose(scene.world_matrix(t), pos, r, s);
    if (rot) *rot = r;
    return true;
}

std::uint32_t depth_of(Scene& scene, Entity e) {
    const auto* h = scene.world().get<scene::Hierarchy>(e);
    return h ? h->depth : 0;
}

// Collects `count` ancestors plus `end`, top first; false when the hierarchy is too shallow.
bool collect_chain(Scene& scene, Entity end, std::uint32_t count, std::vector<Entity>& out) {
    out.assign(count + 1, Entity{});
    Entity e = end;
    for (std::uint32_t i = 0; i <= count; ++i) {
        if (e.is_null() || !scene.world().get<scene::Transform>(e)) return false;
        out[count - i] = e;
        if (i < count) e = scene.parent(e);
    }
    return true;
}

} // namespace

void register_ik_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<IkConstraintComponent>("IKConstraint", "Animation")
            .enumeration(&IkConstraintComponent::solver, "solver", {"two_bone", "chain", "look_at"},
                         {.tooltip = "two_bone: limb (2 parents); chain: FABRIK over chain_length parents; look_at: aim"})
            .field(&IkConstraintComponent::enabled, "enabled")
            .field(&IkConstraintComponent::weight, "weight", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&IkConstraintComponent::target, "target", {.tooltip = "Entity to reach; empty = target_position", .hint = "entity"})
            .field(&IkConstraintComponent::target_position, "target_position", {.speed = 0.01f, .tooltip = "World position (no target entity)"})
            .field(&IkConstraintComponent::pole, "pole", {.tooltip = "two_bone: the knee/elbow bends toward this entity", .hint = "entity"})
            .field(&IkConstraintComponent::match_rotation, "match_rotation", {.tooltip = "End joint takes the target entity's rotation"})
            .field(&IkConstraintComponent::chain_length, "chain_length", {.min = 0, .max = 64, .tooltip = "Parents the solver may rotate"})
            .field(&IkConstraintComponent::iterations, "iterations", {.min = 1, .max = 100})
            .field(&IkConstraintComponent::tolerance, "tolerance", {.min = 0, .max = 1, .speed = 0.0001f, .tooltip = "metres"})
            .field(&IkConstraintComponent::max_extension, "max_extension", {.min = 0.5f, .max = 1, .speed = 0.001f})
            .field(&IkConstraintComponent::aim_axis, "aim_axis", {.speed = 0.01f, .tooltip = "look_at: local axis pointed at the target"})
            .field(&IkConstraintComponent::max_angle, "max_angle", {.min = 0, .max = 180, .speed = 0.5f, .tooltip = "look_at: degrees"})
            .field(&IkConstraintComponent::max_bend, "max_bend",
                   {.min = 0, .max = 180, .speed = 0.5f, .tooltip = "chain: largest bend at each joint, degrees (180 = free)"})
            .commit();
    });
}

IkStats apply_ik(Scene& scene) {
    IkStats stats;
    ecs::World& world = scene.world();
    std::vector<Entity> ends;
    world.each<IkConstraintComponent>([&](Entity e, IkConstraintComponent& c) {
        if (c.enabled && c.weight > 0.0f) ends.push_back(e);
    });
    if (ends.empty()) return stats;
    // Parents first: a spine look-at runs before the arm IK that hangs off it.
    std::stable_sort(ends.begin(), ends.end(), [&](Entity a, Entity b) { return depth_of(scene, a) < depth_of(scene, b); });
    for (const Entity e : ends)
        if (!world.has<IkRuntime>(e)) world.add<IkRuntime>(e);

    std::vector<Joint> joints;
    std::vector<Vec3> before, after;
    std::vector<Quat> world_rot;
    for (const Entity end : ends) {
        const IkConstraintComponent c = *world.get<IkConstraintComponent>(end);
        IkRuntime& rt = *world.get<IkRuntime>(end);
        ++stats.constraints;
        const std::uint32_t count = c.solver == IkSolver::TwoBone ? 2u : c.chain_length;
        Vec3 target;
        Quat target_rot{};
        const bool has_target_rot = c.target != Uuid{};
        if ((c.solver != IkSolver::LookAt && count == 0) || !collect_chain(scene, end, count, rt.joints) ||
            !target_of(scene, c, target, &target_rot)) {
            ++stats.invalid;
            continue;
        }
        const std::size_t n = rt.joints.size();
        // Input pose: the current local rotations, unless they are exactly what IK wrote last time
        // (nothing re-animated them), in which case the remembered input is used.
        const bool cached = rt.input.size() == n && rt.output.size() == n;
        joints.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto* t = world.get<scene::Transform>(rt.joints[i]);
            Quat local = t->rotation;
            if (cached && local == rt.output[i]) local = rt.input[i];
            t->rotation = local;
            joints[i].e = rt.joints[i];
            joints[i].local = local;
        }
        for (std::size_t i = 0; i < n; ++i) {
            Vec3 s;
            decompose(scene.world_matrix(joints[i].e), joints[i].pos, joints[i].rot, s);
        }
        const Quat top_parent = parent_world_rotation(scene, joints[0].e);

        world_rot.resize(n);
        for (std::size_t i = 0; i < n; ++i) world_rot[i] = joints[i].rot;
        bool valid = true;
        if (c.solver == IkSolver::LookAt) {
            const Vec3 aim = joints[n - 1].rot.rotate(length(c.aim_axis) > kEps ? normalize(c.aim_axis) : Vec3{0, 0, 1});
            const Quat total = aim_rotation(joints[n - 1].pos, aim, target, c.max_angle * std::numbers::pi_v<float> / 180.0f);
            // Shared evenly: joint k (top first) gets (k + 1) parts of the turn in world space.
            const Quat part = scale_rotation(total, 1.0f / static_cast<float>(n));
            Quat acc{};
            for (std::size_t i = 0; i < n; ++i) {
                acc = normalize(part * acc);
                world_rot[i] = normalize(acc * joints[i].rot);
            }
        } else {
            before.resize(n);
            for (std::size_t i = 0; i < n; ++i) before[i] = joints[i].pos;
            after = before;
            if (c.solver == IkSolver::TwoBone) {
                Vec3 hint = joints[1].pos;
                if (c.pole != Uuid{}) {
                    const Entity pole = scene.find(c.pole);
                    if (!pole.is_null()) hint = scene.world_matrix(pole).transform_point({});
                }
                const TwoBoneResult r = solve_two_bone(before[0], before[1], before[2], target, hint, c.max_extension);
                if (length(before[1] - before[0]) < kEps || length(before[2] - before[1]) < kEps) valid = false;
                if (!r.reached) ++stats.unreachable;
                after[1] = r.mid;
                after[2] = r.tip;
            } else {
                for (std::size_t i = 0; i + 1 < n; ++i)
                    if (length(before[i + 1] - before[i]) < kEps) valid = false;
                const FabrikResult r = solve_fabrik(after, target, std::max(1u, c.iterations), std::max(0.0f, c.tolerance),
                                                    std::clamp(c.max_bend, 0.0f, 180.0f) * std::numbers::pi_v<float> / 180.0f);
                if (!r.reached) ++stats.unreachable;
            }
            rotations_from_positions(before, after, world_rot);
            if (c.match_rotation && has_target_rot) world_rot[n - 1] = target_rot;
        }
        if (!valid) {
            ++stats.invalid;
            continue;
        }
        // World rotations back to local, blended with the animated pose by weight.
        const float w = std::clamp(c.weight, 0.0f, 1.0f);
        rt.input.resize(n);
        rt.output.resize(n);
        Quat parent = top_parent;
        for (std::size_t i = 0; i < n; ++i) {
            const Quat solved = normalize(parent.conjugate() * world_rot[i]);
            const Quat local = w >= 1.0f ? solved : nlerp(joints[i].local, solved, w);
            auto* t = world.get<scene::Transform>(joints[i].e);
            t->rotation = local;
            rt.input[i] = joints[i].local;
            rt.output[i] = local;
            parent = normalize(parent * local);
        }
        ++stats.solved;
    }
    return stats;
}

void ik_lines(Scene& scene, const std::function<void(Vec3, Vec3)>& emit) {
    std::vector<Entity> chain;
    scene.world().each<IkConstraintComponent>([&](Entity end, IkConstraintComponent& c) {
        if (!c.enabled) return;
        const std::uint32_t count = c.solver == IkSolver::TwoBone ? 2u : c.chain_length;
        if (!collect_chain(scene, end, count, chain)) return;
        for (std::size_t i = 0; i + 1 < chain.size(); ++i)
            emit(scene.world_matrix(chain[i]).transform_point({}), scene.world_matrix(chain[i + 1]).transform_point({}));
        Vec3 t;
        if (!target_of(scene, c, t, nullptr)) return;
        constexpr float s = 0.05f;
        emit(t - Vec3{s, 0, 0}, t + Vec3{s, 0, 0});
        emit(t - Vec3{0, s, 0}, t + Vec3{0, s, 0});
        emit(t - Vec3{0, 0, s}, t + Vec3{0, 0, s});
    });
}

namespace detail {
void restore_ik_input(Scene& scene, Entity end) {
    auto* rt = scene.world().get<IkRuntime>(end);
    if (!rt || rt->input.size() != rt->joints.size() || rt->output.size() != rt->joints.size()) return;
    for (std::size_t i = 0; i < rt->joints.size(); ++i)
        if (auto* t = scene.world().alive(rt->joints[i]) ? scene.world().get<scene::Transform>(rt->joints[i]) : nullptr;
            t && t->rotation == rt->output[i])
            t->rotation = rt->input[i];
}

void set_ik_output_rotation(Scene& scene, Entity end, Entity joint, Quat local) {
    auto* rt = scene.world().get<IkRuntime>(end);
    if (!rt || rt->output.size() != rt->joints.size()) return;
    for (std::size_t i = 0; i < rt->joints.size(); ++i)
        if (rt->joints[i] == joint) rt->output[i] = local;
}
} // namespace detail

} // namespace victus::animation
