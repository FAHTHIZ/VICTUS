// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/animation.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/log.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>

namespace victus::animation {

using namespace math;
using scene::Entity;
using scene::Scene;

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<AnimatorComponent>("Animator", "Animation")
            .field(&AnimatorComponent::model, "model", {.tooltip = "Model asset providing clips", .hint = "model"})
            .field(&AnimatorComponent::clip, "clip", {.tooltip = "Clip to play (state with a controller); changing it cross-fades"})
            .field(&AnimatorComponent::controller, "controller",
                   {.tooltip = "Animation controller (.vanim) relative to the assets folder", .hint = "anim_controller"})
            .field(&AnimatorComponent::playing, "playing")
            .field(&AnimatorComponent::loop, "loop")
            .field(&AnimatorComponent::speed, "speed", {.min = -10, .max = 10, .speed = 0.01f})
            .field(&AnimatorComponent::crossfade, "crossfade", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "seconds"})
            .field(&AnimatorComponent::time, "time", {.min = 0, .max = 1e6f, .speed = 0.01f, .tooltip = "seconds"})
            .field(&AnimatorComponent::apply_root_motion, "apply_root_motion",
                   {.tooltip = "Root-motion states move this entity (off: scripts read entity:root_motion())"})
            .commit();
        scene::Schema<SpriteAnimatorComponent>("SpriteAnimator", "Animation")
            .field(&SpriteAnimatorComponent::sheet, "sheet", {.tooltip = "Sprite sheet (.vsheet) asset", .hint = "spritesheet"})
            .field(&SpriteAnimatorComponent::clip, "clip", {.tooltip = "Clip to play; empty shows 'frame'"})
            .field(&SpriteAnimatorComponent::playing, "playing")
            .field(&SpriteAnimatorComponent::speed, "speed", {.min = -10, .max = 10, .speed = 0.01f})
            .field(&SpriteAnimatorComponent::time, "time", {.min = -1e6f, .max = 1e6f, .speed = 0.01f, .tooltip = "seconds"})
            .field(&SpriteAnimatorComponent::frame, "frame", {.min = 0, .max = 65535, .tooltip = "Sheet frame shown when no clip is set"})
            .commit();
        scene::Schema<SkinnedMeshComponent>("SkinnedMesh", "Animation")
            .field(&SkinnedMeshComponent::skeleton, "skeleton", {.tooltip = "Root entity of the skeleton", .hint = "entity"})
            .field(&SkinnedMeshComponent::skin, "skin", {.min = 0, .max = 1024})
            .commit();
    });
}

namespace {

// Playback of one controller state: accumulated normalized time (1 = one cycle).
struct Playback {
    std::int32_t state{-1};
    float norm{0};
    bool finished{false};
    float last{0}; // norm before the latest advance (exit-time crossings of looping states)
};

// Controller runtime (ADR-0068). Parameters live here, not in the component: they are game
// state the scripts drive every frame, and a saved scene restarts in the default state.
struct ControllerRun {
    std::shared_ptr<const Controller> ctrl;
    std::string path, error;
    std::vector<float> params;
    const AnimationSet* resolved_for{nullptr};
    std::vector<std::vector<std::int32_t>> clips; // state -> point -> set clip index (-1 missing)
    std::vector<std::vector<float>> thresholds;   // Blend1D states
    std::vector<std::vector<Vec2>> points;        // Blend2D states
    Playback cur, prev;
    bool fading{false}, frozen{false};
    float fade_duration{0}, fade_elapsed{0};
    std::vector<NodePose> frozen_pose, scratch;
    std::vector<float> weights;
    std::int32_t root{-1}; // root-motion node index in the set (-1 = none)
};

// Runtime playback state, not serialized: rebuilt from AnimatorComponent after loading.
struct AnimatorState {
    Entity owner;
    Uuid model;
    std::shared_ptr<const AnimationSet> set;
    std::string clip_name;      // clip name last applied (compared with the component)
    std::int32_t current{-1};
    std::int32_t previous{-1};
    float previous_time{0};
    float fade_duration{0}, fade_elapsed{0};
    float pending_fade{-1};     // fade requested by play(); < 0 = component crossfade
    bool finished{false};
    std::vector<Entity> bindings; // node index -> entity
    std::vector<std::uint8_t> animated; // node index -> written by current or previous clip
    std::vector<NodePose> pose, pose_b;
    std::uint32_t bound{0};
    std::size_t scene_size{0}; // entity count when bound: retry missing nodes after it changes
    ControllerRun ctl;
    RootMotion motion; // latest update
};

struct SystemBinding {
    AnimationSystem* system{nullptr};
};

Entity find_child(Scene& scene, Entity parent, std::string_view name) {
    for (const Entity c : scene.children(parent)) {
        const auto* n = scene.world().get<scene::Name>(c);
        if (n && n->value == name) return c;
    }
    return {};
}

Entity find_in_subtree(Scene& scene, Entity root, std::string_view name) {
    std::vector<Entity> stack = scene.children(root);
    std::size_t head = 0;
    while (head < stack.size()) { // breadth-first so the shallowest match wins
        const Entity e = stack[head++];
        const auto* n = scene.world().get<scene::Name>(e);
        if (n && n->value == name) return e;
        for (const Entity c : scene.children(e)) stack.push_back(c);
    }
    return {};
}

bool bindings_valid(Scene& scene, const AnimatorState& s, Entity owner) {
    if (s.owner != owner || s.bindings.size() != (s.set ? s.set->nodes.size() : 0)) return false;
    if (s.bound < s.bindings.size() && s.scene_size != scene.size()) return false;
    for (const Entity e : s.bindings)
        if (e != Entity{} && !scene.world().alive(e)) return false;
    return true;
}

void bind_state(Scene& scene, AnimatorState& s, Entity owner) {
    s.owner = owner;
    s.scene_size = scene.size();
    s.bindings = s.set ? bind_nodes(scene, owner, s.set->nodes) : std::vector<Entity>{};
    s.bound = 0;
    for (const Entity e : s.bindings)
        if (e != Entity{}) ++s.bound;
}

void mark_animated(AnimatorState& s) {
    s.animated.assign(s.set ? s.set->nodes.size() : 0, 0);
    if (!s.set) return;
    for (const std::int32_t ci : {s.current, s.previous}) {
        if (ci < 0) continue;
        for (const Channel& ch : s.set->clips[static_cast<std::size_t>(ci)].channels)
            if (ch.node < s.animated.size()) s.animated[ch.node] = 1;
    }
}

// Applies a clip change on the component to the state (cross-fade from the current clip).
void sync_clip(AnimatorState& s, AnimatorComponent& a) {
    if (s.clip_name == a.clip && (s.current >= 0 || !s.set || s.set->find_clip(a.clip) < 0)) return;
    const std::int32_t next = s.set ? s.set->find_clip(a.clip) : -1;
    const float fade = s.pending_fade >= 0 ? s.pending_fade : a.crossfade;
    s.pending_fade = -1;
    if (s.current >= 0 && next >= 0 && next != s.current && fade > 0.0f) {
        s.previous = s.current;
        s.previous_time = a.time;
        s.fade_duration = fade;
        s.fade_elapsed = 0;
    } else {
        s.previous = -1;
    }
    if (next != s.current) a.time = a.speed < 0.0f && next >= 0 ? s.set->clips[static_cast<std::size_t>(next)].duration : 0.0f;
    s.current = next;
    s.clip_name = a.clip;
    s.finished = false;
    mark_animated(s);
}

struct Work {
    Entity entity;
    AnimatorComponent* a;
    AnimatorState* s;
    std::uint32_t channels{0}, written{0}, transitions{0};
};

// ---- controller runtime ----

float frac(float v) noexcept { return v - std::floor(v); }

void init_controller(AnimatorState& s, std::shared_ptr<const Controller> c, const std::string& path, std::string error) {
    ControllerRun old = std::move(s.ctl);
    s.ctl = ControllerRun{};
    s.ctl.path = path;
    s.ctl.error = std::move(error);
    s.ctl.ctrl = std::move(c);
    if (!s.ctl.ctrl) return;
    const Controller& ctrl = *s.ctl.ctrl;
    s.ctl.params.resize(ctrl.parameters.size());
    for (std::size_t i = 0; i < ctrl.parameters.size(); ++i) {
        s.ctl.params[i] = ctrl.parameters[i].default_value;
        if (old.ctrl) // hot reload keeps the values the game set, by name and type
            if (const std::int32_t o = old.ctrl->find_parameter(ctrl.parameters[i].name);
                o >= 0 && old.ctrl->parameters[static_cast<std::size_t>(o)].type == ctrl.parameters[i].type)
                s.ctl.params[i] = old.params[static_cast<std::size_t>(o)];
    }
    s.ctl.thresholds.assign(ctrl.states.size(), {});
    s.ctl.points.assign(ctrl.states.size(), {});
    for (std::size_t i = 0; i < ctrl.states.size(); ++i) {
        for (const BlendPoint& bp : ctrl.states[i].points) {
            s.ctl.thresholds[i].push_back(bp.at.x);
            s.ctl.points[i].push_back(bp.at);
        }
    }
    s.ctl.cur = {ctrl.default_state, 0, false};
}

// Resolves clip names against the model (serial: logs each missing clip once per binding).
void resolve_controller_clips(AnimatorState& s) {
    ControllerRun& r = s.ctl;
    if (!r.ctrl || r.resolved_for == s.set.get()) return;
    r.resolved_for = s.set.get();
    const Controller& c = *r.ctrl;
    r.clips.assign(c.states.size(), {});
    s.animated.assign(s.set ? s.set->nodes.size() : 0, 0);
    for (std::size_t i = 0; i < c.states.size(); ++i) {
        const ControllerState& st = c.states[i];
        auto add = [&](const std::string& name) {
            const std::int32_t ci = s.set ? s.set->find_clip(name) : -1;
            if (ci < 0 && s.set)
                VICTUS_LOG_WARN("animation", "controller {}: state '{}': model has no clip '{}' (rest pose used)",
                                r.path, st.name, name);
            if (ci >= 0)
                for (const Channel& ch : s.set->clips[static_cast<std::size_t>(ci)].channels)
                    if (ch.node < s.animated.size()) s.animated[ch.node] = 1;
            r.clips[i].push_back(ci);
        };
        if (st.motion == MotionType::Clip) add(st.clip);
        for (const BlendPoint& bp : st.points) add(bp.clip);
    }
    r.root = -1;
    const bool wants_root = std::any_of(c.states.begin(), c.states.end(), [](const ControllerState& st) { return st.root_motion; });
    if (s.set && wants_root) {
        for (std::size_t i = 0; i < s.set->nodes.size() && r.root < 0; ++i)
            if (c.root_node.empty() ? s.set->nodes[i].parent < 0 : s.set->nodes[i].name == c.root_node)
                r.root = static_cast<std::int32_t>(i);
        if (r.root < 0)
            VICTUS_LOG_WARN("animation", "controller {}: model has no root node '{}' (root motion disabled)", r.path,
                            c.root_node);
    }
}

float state_weights(ControllerRun& r, const AnimationSet* set, std::int32_t state);

// ---- root motion ----
// A root-motion state keeps the root node's horizontal position and yaw at their clip-start
// values in the pose and reports how far they travelled instead. Travel is measured on the
// unwrapped cycle: P(n) = floor(n) * (end - start) + value(frac(n)), so looping clips move on
// forever and blends that share a normalized time stay consistent.

float yaw_of(Quat q) noexcept { return 2.0f * std::atan2(q.y, q.w); }

float wrap_angle(float a) noexcept {
    constexpr float kPi = 3.14159265358979f;
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0) a += 2.0f * kPi;
    return a - kPi;
}

struct RootSample {
    Vec3 pos;
    float yaw{0};
};

RootSample root_at(const Clip& clip, const NodePose& rest, std::uint32_t root, float time) noexcept {
    RootSample out{rest.translation, yaw_of(rest.rotation)};
    for (const Channel& ch : clip.channels) {
        if (ch.node != root) continue;
        if (ch.path == Path::Translation) out.pos = sample_vec3(ch, time);
        else if (ch.path == Path::Rotation) out.yaw = yaw_of(sample_quat(ch, time));
    }
    return out;
}

// Unwrapped root travel of a clip at normalized time `n` (cycles).
RootSample root_cycle(const Clip& clip, const NodePose& rest, std::uint32_t root, float n, bool loop) noexcept {
    if (!loop || clip.duration <= 0) return root_at(clip, rest, root, std::clamp(n, 0.0f, 1.0f) * clip.duration);
    const float cycles = std::floor(n);
    const RootSample a = root_at(clip, rest, root, 0), b = root_at(clip, rest, root, clip.duration);
    const RootSample v = root_at(clip, rest, root, (n - cycles) * clip.duration);
    const float per_yaw = wrap_angle(b.yaw - a.yaw);
    return {v.pos + (b.pos - a.pos) * cycles, a.yaw + wrap_angle(v.yaw - a.yaw) + per_yaw * cycles};
}

// Adds `scale` x the state's root travel between pb.last and pb.norm (in the frame the
// character faced at pb.last) to `delta`/`yaw`, and returns whether the state extracts motion.
bool accumulate_root(ControllerRun& r, const AnimationSet& set, const Playback& pb, float scale, Vec3& delta,
                     float& yaw) {
    if (pb.state < 0 || r.root < 0 || scale <= 0) return false;
    const ControllerState& st = r.ctrl->states[static_cast<std::size_t>(pb.state)];
    if (!st.root_motion) return false;
    (void)state_weights(r, &set, pb.state);
    const auto& clips = r.clips[static_cast<std::size_t>(pb.state)];
    const auto root = static_cast<std::uint32_t>(r.root);
    const NodePose& rest = set.nodes[root].rest;
    for (std::size_t k = 0; k < r.weights.size() && k < clips.size(); ++k) {
        const float w = r.weights[k] * scale;
        if (w <= 0 || clips[k] < 0) continue;
        const Clip& clip = set.clips[static_cast<std::size_t>(clips[k])];
        const RootSample start = root_at(clip, rest, root, 0);
        const RootSample p0 = root_cycle(clip, rest, root, pb.last, st.loop);
        const RootSample p1 = root_cycle(clip, rest, root, pb.norm, st.loop);
        Vec3 d = p1.pos - p0.pos;
        d.y = 0; // vertical motion stays in the pose
        // Express the travel relative to where the character faced at p0 (its yaw so far has
        // already turned the entity).
        d = Quat::axis_angle({0, 1, 0}, -(p0.yaw - start.yaw)).rotate(d);
        delta = delta + d * w;
        yaw += (p1.yaw - p0.yaw) * w;
    }
    return true;
}

// Pins a root-motion state's root node to its clip-start horizontal position and yaw.
void pin_root(ControllerRun& r, const AnimationSet& set, const Playback& pb, std::vector<NodePose>& pose) {
    if (pb.state < 0 || r.root < 0 || static_cast<std::size_t>(r.root) >= pose.size()) return;
    const ControllerState& st = r.ctrl->states[static_cast<std::size_t>(pb.state)];
    if (!st.root_motion) return;
    (void)state_weights(r, &set, pb.state);
    const auto& clips = r.clips[static_cast<std::size_t>(pb.state)];
    const auto root = static_cast<std::uint32_t>(r.root);
    const NodePose& rest = set.nodes[root].rest;
    float x = 0, z = 0, start_yaw = 0, total = 0;
    for (std::size_t k = 0; k < r.weights.size() && k < clips.size(); ++k) {
        const float w = r.weights[k];
        if (w <= 0) continue;
        const RootSample s0 = clips[k] >= 0 ? root_at(set.clips[static_cast<std::size_t>(clips[k])], rest, root, 0)
                                            : RootSample{rest.translation, yaw_of(rest.rotation)};
        x += s0.pos.x * w;
        z += s0.pos.z * w;
        start_yaw += s0.yaw * w;
        total += w;
    }
    if (total <= 0) return;
    NodePose& p = pose[root];
    p.translation.x = x / total;
    p.translation.z = z / total;
    p.rotation = normalize(Quat::axis_angle({0, 1, 0}, wrap_angle(start_yaw / total - yaw_of(p.rotation))) * p.rotation);
}

float param_or(const ControllerRun& r, std::string_view name, float fallback) {
    const std::int32_t i = r.ctrl->find_parameter(name);
    return i >= 0 ? r.params[static_cast<std::size_t>(i)] : fallback;
}

float clip_duration(const AnimationSet* set, std::int32_t ci) {
    return set && ci >= 0 ? set->clips[static_cast<std::size_t>(ci)].duration : 0.0f;
}

// Blend weights of `state` (into r.weights) and its effective cycle duration in seconds:
// sum of weight * clip duration / clip speed, so a blended cycle stays phase-synced.
float state_weights(ControllerRun& r, const AnimationSet* set, std::int32_t state) {
    const ControllerState& st = r.ctrl->states[static_cast<std::size_t>(state)];
    const auto si = static_cast<std::size_t>(state);
    const auto& clips = r.clips[si];
    r.weights.assign(clips.size(), 0.0f);
    switch (st.motion) {
    case MotionType::Clip: r.weights.assign(1, 1.0f); break;
    case MotionType::Blend1D: blend_weights_1d(r.thresholds[si], param_or(r, st.param_x, 0), r.weights); break;
    case MotionType::Blend2D:
        blend_weights_2d(r.points[si], {param_or(r, st.param_x, 0), param_or(r, st.param_y, 0)}, r.weights);
        break;
    }
    float d = 0;
    for (std::size_t i = 0; i < clips.size() && i < r.weights.size(); ++i) {
        const float speed = st.points.empty() ? 1.0f : st.points[i].speed;
        const float cd = clip_duration(set, clips[i]);
        d += r.weights[i] * (speed > 0 ? cd / speed : cd);
    }
    return d;
}

void advance_playback(ControllerRun& r, const AnimationSet* set, const AnimatorComponent& a, Playback& pb, float dt) {
    if (pb.state < 0) return;
    pb.last = pb.norm;
    const ControllerState& st = r.ctrl->states[static_cast<std::size_t>(pb.state)];
    const float d = state_weights(r, set, pb.state);
    float rate = st.speed * a.speed;
    if (!st.speed_parameter.empty()) rate *= param_or(r, st.speed_parameter, 1.0f);
    if (!std::isfinite(rate)) rate = 0;
    const float delta = d > 0 ? dt * rate / d : 0.0f;
    if (st.loop) {
        pb.norm += delta;
        if (std::fabs(pb.norm) > 1024.0f) { // keep float precision
            pb.norm = frac(pb.norm);
            pb.last = pb.norm;
        }
        return;
    }
    if (d <= 0) {
        pb.finished = true;
        pb.norm = 1;
        return;
    }
    pb.norm = std::clamp(pb.norm + delta, 0.0f, 1.0f);
    if ((delta > 0 && pb.norm >= 1.0f) || (delta < 0 && pb.norm <= 0.0f)) pb.finished = true;
}

// Samples a state's (blended) pose into `out` (sized to the node count).
void sample_state(ControllerRun& r, const AnimationSet& set, const Playback& pb, std::vector<NodePose>& out,
                  std::uint32_t& channels) {
    const std::size_t n = set.nodes.size();
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = set.nodes[i].rest;
    if (pb.state < 0) return;
    const ControllerState& st = r.ctrl->states[static_cast<std::size_t>(pb.state)];
    (void)state_weights(r, &set, pb.state);
    const auto& clips = r.clips[static_cast<std::size_t>(pb.state)];
    const float phase = st.loop ? frac(pb.norm) : std::clamp(pb.norm, 0.0f, 1.0f);
    std::size_t contributors = 0, only = 0;
    for (std::size_t i = 0; i < r.weights.size(); ++i)
        if (r.weights[i] > 0) {
            ++contributors;
            only = i;
        }
    if (contributors == 1) {
        if (const std::int32_t ci = clips[only]; ci >= 0) {
            const Clip& clip = set.clips[static_cast<std::size_t>(ci)];
            sample(clip, phase * clip.duration, out);
            channels += static_cast<std::uint32_t>(clip.channels.size());
        }
        return;
    }
    // N-way: weighted sum of translation/scale, sign-aligned quaternion sum, renormalized.
    std::vector<NodePose>& tmp = r.scratch;
    tmp.resize(n);
    bool first = true;
    for (std::size_t k = 0; k < r.weights.size(); ++k) {
        const float w = r.weights[k];
        if (w <= 0) continue;
        for (std::size_t i = 0; i < n; ++i) tmp[i] = set.nodes[i].rest;
        if (const std::int32_t ci = clips[k]; ci >= 0) {
            const Clip& clip = set.clips[static_cast<std::size_t>(ci)];
            sample(clip, phase * clip.duration, tmp);
            channels += static_cast<std::uint32_t>(clip.channels.size());
        }
        for (std::size_t i = 0; i < n; ++i) {
            NodePose& o = out[i];
            const NodePose& p = tmp[i];
            Quat q = p.rotation;
            if (first) {
                o.translation = p.translation * w;
                o.scale = p.scale * w;
                o.rotation = {q.x * w, q.y * w, q.z * w, q.w * w};
                continue;
            }
            if (o.rotation.x * q.x + o.rotation.y * q.y + o.rotation.z * q.z + o.rotation.w * q.w < 0.0f)
                q = {-q.x, -q.y, -q.z, -q.w};
            o.translation = o.translation + p.translation * w;
            o.scale = o.scale + p.scale * w;
            o.rotation = {o.rotation.x + q.x * w, o.rotation.y + q.y * w, o.rotation.z + q.z * w, o.rotation.w + q.w * w};
        }
        first = false;
    }
    for (std::size_t i = 0; i < n; ++i) out[i].rotation = normalize(out[i].rotation);
}

// Enters `to` with a cross-fade. A transition fired mid-fade freezes the last output pose as the
// fade source (so the blend never pops), then fades from that snapshot.
void enter_state(AnimatorState& s, AnimatorComponent& a, std::int32_t to, float fade, float offset) {
    ControllerRun& r = s.ctl;
    const std::size_t n = s.set ? s.set->nodes.size() : 0;
    if (fade > 0 && r.cur.state >= 0) {
        if (r.fading) {
            r.frozen = s.pose.size() == n && n > 0;
            if (r.frozen) r.frozen_pose = s.pose;
            r.fading = r.frozen;
        } else {
            r.prev = r.cur;
            r.frozen = false;
            r.fading = true;
        }
        r.fade_duration = fade;
        r.fade_elapsed = 0;
    } else {
        r.fading = r.frozen = false;
    }
    r.cur = {to, offset, false, offset};
    a.clip = r.ctrl->states[static_cast<std::size_t>(to)].name;
    s.clip_name = a.clip;
    s.finished = false;
}

bool condition_holds(const ControllerRun& r, const Condition& c) {
    const float v = r.params[static_cast<std::size_t>(c.index)];
    switch (c.op) {
    case ConditionOp::True:
    case ConditionOp::Trigger: return v != 0.0f;
    case ConditionOp::False: return v == 0.0f;
    case ConditionOp::Greater: return v > c.value;
    case ConditionOp::GreaterEqual: return v >= c.value;
    case ConditionOp::Less: return v < c.value;
    case ConditionOp::LessEqual: return v <= c.value;
    case ConditionOp::Equal: return v == c.value;
    case ConditionOp::NotEqual: return v != c.value;
    }
    return false;
}

// First transition that can fire: any-state ones, then the current state's, in authored order.
const ControllerTransition* pick_transition(const ControllerRun& r) {
    const Controller& c = *r.ctrl;
    for (int pass = 0; pass < 2; ++pass) {
        for (const ControllerTransition& t : c.transitions) {
            if (pass == 0 ? t.from >= 0 : t.from != r.cur.state) continue;
            if (t.from < 0 && t.to == r.cur.state && !t.allow_self) continue;
            if (t.exit_time >= 0) {
                const bool loop = c.states[static_cast<std::size_t>(r.cur.state)].loop;
                if (loop && t.exit_time < 1.0f) {
                    // Evaluated every cycle, on the frame its point in the cycle is crossed (as Unity).
                    if (std::floor(r.cur.norm - t.exit_time) == std::floor(r.cur.last - t.exit_time)) continue;
                } else if (r.cur.norm < t.exit_time) {
                    continue; // reached once; stays satisfied while the conditions are pending
                }
            }
            bool ok = true;
            for (const Condition& cond : t.conditions)
                if (!condition_holds(r, cond)) {
                    ok = false;
                    break;
                }
            if (ok) return &t;
        }
    }
    return nullptr;
}

float advance(float time, float delta, float duration, bool loop, bool& finished) {
    const float t = time + delta;
    if (loop) return wrap_time(t, duration, true);
    if (duration <= 0.0f) {
        finished = true;
        return 0.0f;
    }
    if (t >= duration && delta > 0.0f) finished = true;
    if (t <= 0.0f && delta < 0.0f) finished = true;
    return std::clamp(t, 0.0f, duration);
}

void write_pose(Scene& scene, Work& w) {
    AnimatorState& s = *w.s;
    const std::size_t n = std::min({s.pose.size(), s.animated.size(), s.bindings.size()});
    for (std::size_t i = 0; i < n; ++i) {
        if (!s.animated[i] || s.bindings[i] == Entity{}) continue;
        auto* t = scene.world().get<scene::Transform>(s.bindings[i]);
        if (!t) continue;
        t->position = s.pose[i].translation;
        t->rotation = s.pose[i].rotation;
        t->scale = s.pose[i].scale;
        ++w.written;
    }
}

void evaluate_controller(Scene& scene, Work& w, float dt) {
    AnimatorState& s = *w.s;
    AnimatorComponent& a = *w.a;
    ControllerRun& r = s.ctl;
    const AnimationSet& set = *s.set;
    const float step = a.playing ? dt : 0.0f;
    advance_playback(r, &set, a, r.cur, step);
    if (r.fading && !r.frozen) advance_playback(r, &set, a, r.prev, step);
    if (a.playing) {
        if (const ControllerTransition* t = pick_transition(r)) {
            for (const Condition& cond : t->conditions)
                if (cond.op == ConditionOp::Trigger) r.params[static_cast<std::size_t>(cond.index)] = 0;
            enter_state(s, a, t->to, t->duration, t->offset);
            ++w.transitions;
        }
    }
    sample_state(r, set, r.cur, s.pose, w.channels);
    pin_root(r, set, r.cur, s.pose);
    float fade_weight = 1.0f;
    if (r.fading)
        fade_weight = r.fade_duration > 0.0f ? std::clamp((r.fade_elapsed + std::abs(dt)) / r.fade_duration, 0.0f, 1.0f) : 1.0f;
    // Root motion of this step: the current state (and the fading-out one, unless frozen), weighted
    // by the cross-fade. A state entered by a transition this step starts at its offset (no travel).
    {
        Vec3 delta{};
        float yaw = 0;
        bool active = !w.transitions && accumulate_root(r, set, r.cur, fade_weight, delta, yaw);
        if (r.fading && !r.frozen) active = accumulate_root(r, set, r.prev, 1.0f - fade_weight, delta, yaw) || active;
        s.motion = {};
        s.motion.active = active;
        if (active) {
            auto* t = scene.world().get<scene::Transform>(w.entity);
            const Quat rot = t ? t->rotation : Quat{};
            const Vec3 scl = t ? t->scale : Vec3{1, 1, 1};
            s.motion.position = rot.rotate(Vec3{delta.x * scl.x, delta.y * scl.y, delta.z * scl.z});
            s.motion.yaw = yaw;
            if (a.apply_root_motion && t) {
                t->position = t->position + s.motion.position;
                t->rotation = normalize(t->rotation * Quat::axis_angle({0, 1, 0}, yaw));
                s.motion.applied = true;
            }
        }
    }
    if (r.fading) {
        const std::vector<NodePose>* src = &r.frozen_pose;
        if (!r.frozen) {
            sample_state(r, set, r.prev, s.pose_b, w.channels);
            pin_root(r, set, r.prev, s.pose_b);
            src = &s.pose_b;
        }
        r.fade_elapsed += std::abs(dt);
        const float weight = fade_weight;
        for (std::size_t i = 0; i < s.pose.size() && i < src->size(); ++i)
            s.pose[i] = blend((*src)[i], s.pose[i], weight);
        if (weight >= 1.0f) {
            r.fading = r.frozen = false;
            r.frozen_pose.clear();
        }
    }
    s.finished = r.cur.finished;
    a.time = (r.ctrl->states[static_cast<std::size_t>(r.cur.state)].loop ? frac(r.cur.norm) : r.cur.norm) *
             state_weights(r, &set, r.cur.state);
    write_pose(scene, w);
}

void evaluate(Scene& scene, Work& w, float dt) {
    AnimatorState& s = *w.s;
    AnimatorComponent& a = *w.a;
    if (s.set && s.ctl.ctrl && s.ctl.cur.state >= 0) {
        evaluate_controller(scene, w, dt);
        return;
    }
    s.motion = {};
    if (!s.set || s.current < 0) return;
    const Clip& clip = s.set->clips[static_cast<std::size_t>(s.current)];
    const float delta = a.playing ? dt * a.speed : 0.0f;
    if (a.playing) {
        s.finished = false;
        a.time = advance(a.time, delta, clip.duration, a.loop, s.finished);
    } else {
        a.time = wrap_time(a.time, clip.duration, a.loop);
    }

    const std::size_t n = s.set->nodes.size();
    s.pose.resize(n);
    for (std::size_t i = 0; i < n; ++i) s.pose[i] = s.set->nodes[i].rest;
    sample(clip, a.time, s.pose);
    w.channels += static_cast<std::uint32_t>(clip.channels.size());

    if (s.previous >= 0) {
        const Clip& prev = s.set->clips[static_cast<std::size_t>(s.previous)];
        bool prev_done = false;
        if (a.playing) s.previous_time = advance(s.previous_time, delta, prev.duration, a.loop, prev_done);
        s.pose_b.resize(n);
        for (std::size_t i = 0; i < n; ++i) s.pose_b[i] = s.set->nodes[i].rest;
        sample(prev, s.previous_time, s.pose_b);
        w.channels += static_cast<std::uint32_t>(prev.channels.size());
        s.fade_elapsed += std::abs(dt);
        const float weight = s.fade_duration > 0.0f ? std::clamp(s.fade_elapsed / s.fade_duration, 0.0f, 1.0f) : 1.0f;
        for (std::size_t i = 0; i < n; ++i)
            if (s.animated[i]) s.pose[i] = blend(s.pose_b[i], s.pose[i], weight);
        if (weight >= 1.0f) {
            s.previous = -1;
            mark_animated(s);
        }
    }

    write_pose(scene, w);
}

} // namespace

std::vector<Entity> bind_nodes(Scene& scene, Entity root, std::span<const AnimationSet::Node> nodes) {
    std::vector<Entity> out(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        Entity e;
        const bool parent_known = node.parent >= 0 && static_cast<std::size_t>(node.parent) < i;
        const Entity parent = node.parent < 0 ? root : (parent_known ? out[static_cast<std::size_t>(node.parent)] : Entity{});
        if (parent != Entity{}) e = find_child(scene, parent, node.name);
        if (e == Entity{}) e = find_in_subtree(scene, root, node.name);
        out[i] = e;
    }
    return out;
}

struct AnimationSystem::Impl {
    AnimationSettings settings;
    std::unordered_map<Uuid, std::shared_ptr<const AnimationSet>, UuidHash> sets;     // explicit
    std::unordered_map<Uuid, std::shared_ptr<const AnimationSet>, UuidHash> cache;    // from provider
    std::vector<Work> work;
    std::unordered_map<Uuid, std::shared_ptr<const SpriteSheet>, UuidHash> explicit_sheets;
    std::unordered_map<Uuid, std::shared_ptr<const SpriteSheet>, UuidHash> sheet_cache;
    struct CachedController {
        std::shared_ptr<const Controller> ctrl;
        std::string error;
    };
    std::unordered_map<std::string, std::shared_ptr<const Controller>> explicit_controllers;
    std::unordered_map<std::string, CachedController> controller_cache;

    std::shared_ptr<const Controller> resolve_controller(const std::string& path, std::string& error) {
        error.clear();
        if (path.empty()) return nullptr;
        if (const auto it = explicit_controllers.find(path); it != explicit_controllers.end()) return it->second;
        auto it = controller_cache.find(path);
        if (it == controller_cache.end()) {
            CachedController c;
            if (!settings.controllers) {
                c.error = std::format("{}: no animation controller loader", path);
            } else if (auto text = settings.controllers(path); !text) {
                c.error = std::format("{}: {}", path, text.error().message);
            } else if (auto ctrl = Controller::parse(*text, path); !ctrl) {
                c.error = std::format("{}: {}", path, ctrl.error().message);
            } else {
                c.ctrl = std::move(*ctrl);
            }
            if (!c.error.empty()) VICTUS_LOG_ERROR("animation", "{}", c.error);
            it = controller_cache.emplace(path, std::move(c)).first;
        }
        error = it->second.error;
        return it->second.ctrl;
    }

    std::shared_ptr<const SpriteSheet> resolve_sheet(const Uuid& id) {
        if (id.is_nil()) return nullptr;
        if (const auto it = explicit_sheets.find(id); it != explicit_sheets.end()) return it->second;
        if (const auto it = sheet_cache.find(id); it != sheet_cache.end()) return it->second;
        auto s = settings.sheets ? settings.sheets(id) : nullptr;
        sheet_cache.emplace(id, s);
        return s;
    }

    std::shared_ptr<const AnimationSet> resolve(const Uuid& model) {
        if (const auto it = sets.find(model); it != sets.end()) return it->second;
        if (const auto it = cache.find(model); it != cache.end()) return it->second;
        std::shared_ptr<const AnimationSet> set = settings.provider ? settings.provider(model) : nullptr;
        if (set) {
            for (const Clip& clip : set->clips) {
                if (auto st = validate(clip, set->nodes.size()); !st) {
                    VICTUS_LOG_WARN("animation", "model {}: {}", model.to_string(), st.error().message);
                    set = nullptr;
                    break;
                }
            }
        }
        cache.emplace(model, set);
        return set;
    }

    // Ensures `s` reflects the component (model, bindings, clip). Serial: may read the hierarchy.
    void prepare(Scene& scene, Entity e, AnimatorComponent& a, AnimatorState& s) {
        if (s.model != a.model || !s.set) {
            auto set = resolve(a.model);
            if (set != s.set || s.model != a.model) {
                s = AnimatorState{};
                s.model = a.model;
                s.set = std::move(set);
            }
        }
        if (!bindings_valid(scene, s, e)) bind_state(scene, s, e);
        if (!a.controller.empty()) {
            std::string error;
            auto ctrl = resolve_controller(a.controller, error);
            if (ctrl != s.ctl.ctrl || s.ctl.path != a.controller) {
                init_controller(s, ctrl, a.controller, std::move(error));
                if (s.ctl.ctrl) {
                    // Start in the state the clip field names (saved scenes, hot reload), else the default.
                    if (const std::int32_t st = s.ctl.ctrl->find_state(a.clip); st >= 0) s.ctl.cur.state = st;
                    a.clip = s.ctl.ctrl->states[static_cast<std::size_t>(s.ctl.cur.state)].name;
                    s.previous = -1;
                    s.current = -1;
                    s.clip_name = a.clip;
                    a.time = 0;
                }
            }
        } else if (!s.ctl.path.empty()) {
            s.ctl = ControllerRun{}; // controller removed: back to plain clip playback
            s.clip_name.clear();
            s.current = -1;
            mark_animated(s);
        }
        if (s.ctl.ctrl && s.set) {
            resolve_controller_clips(s);
            ControllerRun& r = s.ctl;
            const std::string& cur = r.ctrl->states[static_cast<std::size_t>(r.cur.state)].name;
            if (a.clip != cur) { // the clip field names the state to enter
                const std::int32_t to = r.ctrl->find_state(a.clip);
                const float fade = s.pending_fade >= 0 ? s.pending_fade : a.crossfade;
                if (to >= 0 && !a.clip.empty() && s.clip_name == cur) enter_state(s, a, to, fade, 0.0f);
                else a.clip = cur;
            }
            s.clip_name = a.clip;
            s.pending_fade = -1;
            return;
        }
        sync_clip(s, a);
    }

    // Ensures the entity has an Animator state prepared this frame; null without an Animator.
    AnimatorState* ensure(Scene& scene, Entity e, AnimatorComponent*& a) {
        a = scene.world().alive(e) ? scene.world().get<AnimatorComponent>(e) : nullptr;
        if (!a) return nullptr;
        auto* s = scene.world().get<AnimatorState>(e);
        if (!s) {
            scene.world().add<AnimatorState>(e);
            a = scene.world().get<AnimatorComponent>(e);
            s = scene.world().get<AnimatorState>(e);
        }
        prepare(scene, e, *a, *s);
        return s;
    }
};

AnimationSystem::AnimationSystem(AnimationSettings settings) : impl_(std::make_unique<Impl>()) {
    register_components();
    impl_->settings = std::move(settings);
}
AnimationSystem::~AnimationSystem() = default;

void AnimationSystem::add_set(const Uuid& model, std::shared_ptr<const AnimationSet> set) { impl_->sets[model] = std::move(set); }
void AnimationSystem::invalidate(const Uuid& model) {
    impl_->cache.erase(model);
}
void AnimationSystem::clear_cache() { impl_->cache.clear(); }
std::shared_ptr<const AnimationSet> AnimationSystem::set(const Uuid& model) { return impl_->resolve(model); }

void AnimationSystem::update(Scene& scene, float dt) {
    update_sprites(scene, dt);
    ecs::World& world = scene.world();
    // Structural changes first: add state to new animators, drop state of removed ones.
    std::vector<Entity> missing, orphans;
    world.each<AnimatorComponent>([&](Entity e, AnimatorComponent&) {
        if (!world.has<AnimatorState>(e)) missing.push_back(e);
    });
    world.each_excluding<AnimatorState>(ecs::World::Exclude<AnimatorComponent>{}, [&](Entity e, AnimatorState&) { orphans.push_back(e); });
    for (const Entity e : missing) world.add<AnimatorState>(e);
    for (const Entity e : orphans) world.remove<AnimatorState>(e);

    auto& work = impl_->work;
    work.clear();
    world.each<AnimatorComponent, AnimatorState>([&](Entity e, AnimatorComponent& a, AnimatorState& s) { work.push_back({e, &a, &s}); });

    stats_ = {};
    stats_.animators = static_cast<std::uint32_t>(work.size());
    for (Work& w : work) {
        impl_->prepare(scene, w.entity, *w.a, *w.s);
        if (!w.s->set) ++stats_.missing_models;
    }
    jobs::JobSystem* js = impl_->settings.jobs;
    if (js && work.size() >= impl_->settings.parallel_threshold) {
        js->parallel_for(0, work.size(), 8, [&](std::size_t b, std::size_t end) {
            for (std::size_t i = b; i < end; ++i) evaluate(scene, work[i], dt);
        });
    } else {
        for (Work& w : work) evaluate(scene, w, dt);
    }
    for (const Work& w : work) {
        stats_.channels_sampled += w.channels;
        stats_.nodes_written += w.written;
        stats_.transitions += w.transitions;
        if (w.s->ctl.ctrl) ++stats_.controlled;
        else if (!w.a->controller.empty()) ++stats_.controller_errors;
    }
}

bool AnimationSystem::update_entity(Scene& scene, Entity e, float dt) {
    AnimatorComponent* a = nullptr;
    AnimatorState* s = impl_->ensure(scene, e, a);
    if (!s) return false;
    Work w{e, a, s};
    evaluate(scene, w, dt);
    stats_ = {};
    stats_.animators = 1;
    stats_.missing_models = s->set ? 0 : 1;
    stats_.channels_sampled = w.channels;
    stats_.nodes_written = w.written;
    stats_.transitions = w.transitions;
    if (s->ctl.ctrl) stats_.controlled = 1;
    else if (!a->controller.empty()) stats_.controller_errors = 1;
    return true;
}

RootMotion AnimationSystem::root_motion(Scene& scene, Entity e) {
    if (!scene.world().alive(e)) return {};
    const auto* s = scene.world().get<AnimatorState>(e);
    return s ? s->motion : RootMotion{};
}

bool AnimationSystem::play(Scene& scene, Entity e, std::string_view clip, float fade, bool restart) {
    auto* a = scene.world().alive(e) ? scene.world().get<AnimatorComponent>(e) : nullptr;
    if (!a) return false;
    // Controller mode: `clip` names a state. A controller that failed to load falls back to
    // plain clip playback below, as evaluation does.
    if (AnimatorState* s = a->controller.empty() ? nullptr : impl_->ensure(scene, e, a); s && s->ctl.ctrl) {
        const std::int32_t to = s->ctl.ctrl->find_state(clip);
        if (to < 0 || !s->set) return false;
        a->playing = true;
        if (to == s->ctl.cur.state && !restart) return true;
        enter_state(*s, *a, to, fade < 0 ? a->crossfade : fade, 0.0f);
        return true;
    }
    auto set = impl_->resolve(a->model);
    if (!set || set->find_clip(clip) < 0) return false;
    auto* s = scene.world().get<AnimatorState>(e);
    if (!s) {
        scene.world().add<AnimatorState>(e);
        a = scene.world().get<AnimatorComponent>(e);
        s = scene.world().get<AnimatorState>(e);
    }
    a->playing = true;
    if (a->clip == clip) {
        if (restart) {
            a->time = a->speed < 0.0f ? set->clips[static_cast<std::size_t>(set->find_clip(clip))].duration : 0.0f;
            s->finished = false;
        }
        return true;
    }
    a->clip = std::string(clip);
    s->pending_fade = fade;
    impl_->prepare(scene, e, *a, *s);
    if (!restart && s->previous >= 0) a->time = wrap_time(s->previous_time, set->clips[static_cast<std::size_t>(s->current)].duration, a->loop);
    return true;
}

AnimatorStatus AnimationSystem::status(Scene& scene, Entity e) {
    AnimatorStatus st;
    auto* a = scene.world().alive(e) ? scene.world().get<AnimatorComponent>(e) : nullptr;
    if (!a) return st;
    if (auto* s = scene.world().get<AnimatorState>(e); s && s->ctl.ctrl && s->set && s->ctl.cur.state >= 0) {
        ControllerRun& r = s->ctl;
        st.valid = true;
        st.controlled = true;
        st.state = r.ctrl->states[static_cast<std::size_t>(r.cur.state)].name;
        st.clip = st.state;
        st.normalized_time = r.cur.norm;
        st.time = a->time;
        st.duration = state_weights(r, s->set.get(), r.cur.state);
        st.playing = a->playing;
        st.looping = r.ctrl->states[static_cast<std::size_t>(r.cur.state)].loop;
        st.finished = r.cur.finished;
        st.bound_nodes = s->bound;
        for (const auto v : s->animated) st.animated_nodes += v;
        if (r.fading && r.fade_duration > 0.0f) st.blend = std::clamp(r.fade_elapsed / r.fade_duration, 0.0f, 1.0f);
        return st;
    }
    st.clip = a->clip;
    st.time = a->time;
    st.playing = a->playing;
    st.looping = a->loop;
    auto set = impl_->resolve(a->model);
    st.valid = set != nullptr;
    if (set) {
        if (const std::int32_t ci = set->find_clip(a->clip); ci >= 0) st.duration = set->clips[static_cast<std::size_t>(ci)].duration;
    }
    if (const auto* s = scene.world().get<AnimatorState>(e)) {
        st.finished = s->finished;
        st.bound_nodes = s->bound;
        for (const auto v : s->animated) st.animated_nodes += v;
        if (s->previous >= 0 && s->fade_duration > 0.0f) st.blend = std::clamp(s->fade_elapsed / s->fade_duration, 0.0f, 1.0f);
    }
    return st;
}

std::vector<std::string> AnimationSystem::clips(Scene& scene, Entity e) {
    std::vector<std::string> out;
    const auto* a = scene.world().get<AnimatorComponent>(e);
    if (!a) return out;
    if (auto set = impl_->resolve(a->model))
        for (const Clip& c : set->clips) out.push_back(c.name);
    return out;
}

void AnimationSystem::add_controller(std::string path, std::shared_ptr<const Controller> controller) {
    impl_->explicit_controllers[std::move(path)] = std::move(controller);
}
void AnimationSystem::reload_controllers() { impl_->controller_cache.clear(); }
std::shared_ptr<const Controller> AnimationSystem::controller(std::string_view path, std::string* error) {
    std::string err;
    auto c = impl_->resolve_controller(std::string(path), err);
    if (error) *error = std::move(err);
    return c;
}

namespace {
ControllerRun* run_of(AnimatorState* s) { return s && s->ctl.ctrl ? &s->ctl : nullptr; }
} // namespace

bool AnimationSystem::set_parameter(Scene& scene, Entity e, std::string_view name, float value) {
    AnimatorComponent* a = nullptr;
    ControllerRun* r = run_of(impl_->ensure(scene, e, a));
    if (!r) return false;
    const std::int32_t i = r->ctrl->find_parameter(name);
    if (i < 0) return false;
    if (!std::isfinite(value)) value = 0;
    switch (r->ctrl->parameters[static_cast<std::size_t>(i)].type) {
    case ParamType::Float: break;
    case ParamType::Int: value = std::round(value); break;
    case ParamType::Bool:
    case ParamType::Trigger: value = value != 0.0f ? 1.0f : 0.0f; break;
    }
    r->params[static_cast<std::size_t>(i)] = value;
    return true;
}

bool AnimationSystem::set_trigger(Scene& scene, Entity e, std::string_view name) {
    AnimatorComponent* a = nullptr;
    ControllerRun* r = run_of(impl_->ensure(scene, e, a));
    if (!r) return false;
    const std::int32_t i = r->ctrl->find_parameter(name);
    if (i < 0 || r->ctrl->parameters[static_cast<std::size_t>(i)].type != ParamType::Trigger) return false;
    r->params[static_cast<std::size_t>(i)] = 1;
    return true;
}

bool AnimationSystem::reset_trigger(Scene& scene, Entity e, std::string_view name) {
    AnimatorComponent* a = nullptr;
    ControllerRun* r = run_of(impl_->ensure(scene, e, a));
    if (!r) return false;
    const std::int32_t i = r->ctrl->find_parameter(name);
    if (i < 0 || r->ctrl->parameters[static_cast<std::size_t>(i)].type != ParamType::Trigger) return false;
    r->params[static_cast<std::size_t>(i)] = 0;
    return true;
}

std::optional<float> AnimationSystem::parameter(Scene& scene, Entity e, std::string_view name) {
    AnimatorComponent* a = nullptr;
    ControllerRun* r = run_of(impl_->ensure(scene, e, a));
    if (!r) return std::nullopt;
    const std::int32_t i = r->ctrl->find_parameter(name);
    if (i < 0) return std::nullopt;
    return r->params[static_cast<std::size_t>(i)];
}

ControllerStatus AnimationSystem::controller_status(Scene& scene, Entity e) {
    ControllerStatus out;
    AnimatorComponent* a = nullptr;
    AnimatorState* s = impl_->ensure(scene, e, a);
    if (!s) return out;
    out.controller = a->controller;
    out.error = s->ctl.error;
    ControllerRun* r = run_of(s);
    if (!r) return out;
    const Controller& c = *r->ctrl;
    out.valid = true;
    for (const ControllerState& st : c.states) {
        out.states.push_back(st.name);
        out.root_motion_states.push_back(st.root_motion ? 1 : 0);
    }
    if (s->set && r->root >= 0 && static_cast<std::size_t>(r->root) < s->set->nodes.size())
        out.root_node = s->set->nodes[static_cast<std::size_t>(r->root)].name;
    for (std::size_t i = 0; i < c.parameters.size(); ++i)
        out.parameters.push_back({c.parameters[i].name, c.parameters[i].type, r->params[i]});
    if (r->cur.state < 0) return out;
    out.state = c.states[static_cast<std::size_t>(r->cur.state)].name;
    out.normalized_time = r->cur.norm;
    if (r->fading) {
        out.blend = r->fade_duration > 0 ? std::clamp(r->fade_elapsed / r->fade_duration, 0.0f, 1.0f) : 1.0f;
        out.previous_state = r->frozen ? std::string("(blend)") : c.states[static_cast<std::size_t>(r->prev.state)].name;
    }
    if (!s->set) return out;
    resolve_controller_clips(*s);
    (void)state_weights(*r, s->set.get(), r->cur.state);
    const ControllerState& st = c.states[static_cast<std::size_t>(r->cur.state)];
    const auto& clips = r->clips[static_cast<std::size_t>(r->cur.state)];
    for (std::size_t i = 0; i < clips.size() && i < r->weights.size(); ++i)
        out.weights.push_back({st.motion == MotionType::Clip ? st.clip : st.points[i].clip, r->weights[i], clips[i] < 0});
    return out;
}

ControllerLoader directory_loader(std::filesystem::path assets_root) {
    return [root = std::move(assets_root)](std::string_view path) -> Result<std::string> {
        auto clean = fs::sanitize_relative(path);
        if (!clean) return std::move(clean).error();
        return fs::read_text(root / *clean, 16u * 1024u * 1024u);
    };
}

void bind(Scene& scene, AnimationSystem* system) {
    if (auto* b = scene.world().resource<SystemBinding>()) b->system = system;
    else scene.world().set_resource<SystemBinding>().system = system;
}

AnimationSystem* system_of(Scene& scene) {
    const auto* b = scene.world().resource<SystemBinding>();
    return b ? b->system : nullptr;
}

void add_animation_systems(gameplay::Simulation& sim, AnimationSystem& system) {
    AnimationSystem* sys = &system;
    sim.add_system("animation.update", gameplay::Phase::Update, [sys](Scene& scene, const gameplay::FrameTime& t) {
        if (system_of(scene) != sys) bind(scene, sys);
        sys->update(scene, static_cast<float>(t.dt));
    });
}

void skeleton_lines(Scene& scene, const std::function<void(Vec3, Vec3)>& emit) {
    scene.world().each<AnimatorState>([&](Entity, AnimatorState& s) {
        if (!s.set) return;
        for (std::size_t i = 0; i < s.bindings.size() && i < s.set->nodes.size(); ++i) {
            const std::int32_t p = s.set->nodes[i].parent;
            if (p < 0 || static_cast<std::size_t>(p) >= s.bindings.size()) continue;
            const Entity a = s.bindings[i], b = s.bindings[static_cast<std::size_t>(p)];
            if (a == Entity{} || b == Entity{}) continue;
            const auto* wa = scene.world().get<scene::WorldTransform>(a);
            const auto* wb = scene.world().get<scene::WorldTransform>(b);
            if (!wa || !wb) continue;
            emit(wb->matrix.transform_point({}), wa->matrix.transform_point({}));
        }
    });
}

} // namespace victus::animation

// ---- sprite animation ------------------------------------------------------------------------

namespace victus::animation {

namespace {
// Runtime state: the clip last applied, to restart time when the component's clip changes.
struct SpriteAnimatorState {
    std::string clip;
    bool started{false};
};
} // namespace

void AnimationSystem::add_sheet(const Uuid& id, std::shared_ptr<const SpriteSheet> data) { impl_->explicit_sheets[id] = std::move(data); }
void AnimationSystem::clear_sheets() { impl_->sheet_cache.clear(); }
std::shared_ptr<const SpriteSheet> AnimationSystem::sheet(const Uuid& id) { return impl_->resolve_sheet(id); }

void AnimationSystem::update_sprites(Scene& scene, float dt) {
    auto& w = scene.world();
    std::vector<Entity> fresh;
    w.each<SpriteAnimatorComponent>([&](Entity e, SpriteAnimatorComponent& a) {
        auto* sp = w.get<scene::SpriteComponent>(e);
        if (!sp) return;
        const auto sheet = impl_->resolve_sheet(a.sheet);
        if (!sheet) return;
        auto* st = w.get<SpriteAnimatorState>(e);
        if (!st) {
            fresh.push_back(e);
        } else if (st->clip != a.clip) {
            if (st->started) a.time = 0; // a clip change restarts; a loaded time is kept
            st->clip = a.clip;
        }
        if (st) st->started = true;
        if (!sheet->texture.is_nil()) sp->texture_asset = sheet->texture;
        const SpriteClip* clip = a.clip.empty() ? nullptr : sheet->find(a.clip);
        if (!clip) {
            sp->uv = sheet->frame_uv(a.frame);
            return;
        }
        if (a.playing && dt != 0) {
            a.time += dt * a.speed;
            // Keep looping clocks small so float precision never degrades.
            const float dur = clip->duration();
            if (clip->loop && dur > 0 && std::fabs(a.time) > dur * 1024) a.time = std::fmod(a.time, dur);
            if (!clip->loop && dur > 0) a.time = std::clamp(a.time, 0.0f, dur);
        }
        sp->uv = sheet->frame_uv(clip->frames[clip_frame_at(*clip, a.time)]);
    });
    for (const Entity e : fresh) {
        auto& st = w.add<SpriteAnimatorState>(e);
        st.clip = w.get<SpriteAnimatorComponent>(e)->clip;
        st.started = true;
    }
}

bool AnimationSystem::play_sprite(Scene& scene, Entity e, std::string_view clip, bool restart) {
    auto& w = scene.world();
    auto* a = w.alive(e) ? w.get<SpriteAnimatorComponent>(e) : nullptr;
    if (!a) return false;
    const auto sheet = impl_->resolve_sheet(a->sheet);
    if (!sheet || !sheet->find(clip)) return false;
    if (a->clip != clip || restart) a->time = 0;
    a->clip = std::string(clip);
    a->playing = true;
    if (auto* st = w.get<SpriteAnimatorState>(e)) st->clip = a->clip; // time already reset
    return true;
}

SpriteAnimatorStatus AnimationSystem::sprite_status(Scene& scene, Entity e) {
    SpriteAnimatorStatus out;
    auto& w = scene.world();
    const auto* a = w.alive(e) ? w.get<SpriteAnimatorComponent>(e) : nullptr;
    if (!a) return out;
    const auto sheet = impl_->resolve_sheet(a->sheet);
    if (!sheet) return out;
    out.valid = true;
    out.playing = a->playing;
    out.time = a->time;
    const SpriteClip* clip = a->clip.empty() ? nullptr : sheet->find(a->clip);
    if (!clip) {
        out.frame = std::min(a->frame, sheet->frame_count() - 1);
        return out;
    }
    out.clip = clip->name;
    out.duration = clip->duration();
    out.looping = clip->loop;
    out.clip_frame = clip_frame_at(*clip, a->time);
    out.frame = clip->frames[out.clip_frame];
    out.finished = !clip->loop && (a->speed >= 0 ? a->time >= out.duration : a->time <= 0);
    return out;
}

std::vector<std::string> AnimationSystem::sprite_clips(Scene& scene, Entity e) {
    std::vector<std::string> out;
    auto& w = scene.world();
    const auto* a = w.alive(e) ? w.get<SpriteAnimatorComponent>(e) : nullptr;
    if (!a) return out;
    if (const auto sheet = impl_->resolve_sheet(a->sheet))
        for (const auto& c : sheet->clips) out.push_back(c.name);
    return out;
}

} // namespace victus::animation
