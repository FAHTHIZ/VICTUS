// SPDX-License-Identifier: Apache-2.0
// AiSystem: perception, behaviour tree execution and registration.
#include "victus/ai/ai.hpp"
#include "victus/gameplay/sim_lod.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <numbers>
#include <unordered_set>

namespace victus::ai {

using scene::Scene;

namespace {

struct NodeState {
    bool running{false};
    bool done{false}; // Parallel children: finished in the current activation
    NodeStatus last{NodeStatus::Idle};
    std::uint64_t visit{0};
    std::uint32_t child{0};
    std::uint32_t counter{0};
    double start{0};
    double until{0};
    double cooldown_until{-1e300}; // survives activations
    math::Vec3 dest;
    bool dest_set{false};
};

struct Instance {
    std::shared_ptr<const BehaviorTree> tree;
    bool override_tree{false};
    std::string path;  // asset the tree came from
    std::string error;
    std::vector<NodeState> st;
    std::uint64_t tick_id{0};
    double time{0};
    float acc{0};
    Random rng;
    std::uint64_t ticks{0};
    std::uint32_t completions{0};
    NodeStatus last_root{NodeStatus::Idle};
    bool seen{false}; // still has a component (per update)
    bool seeded{false};
};

struct Noise {
    math::Vec3 position;
    float loudness;
    Entity source;
};

struct CachedTree {
    std::shared_ptr<const BehaviorTree> tree;
    std::string error;
};

math::Vec3 world_position(const Scene& s, Entity e) { return s.world_matrix(e).transform_point({}); }

bool approx(double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }

bool equal(const Value& a, const Value& b) {
    if (const double* x = std::get_if<double>(&a))
        if (const double* y = std::get_if<double>(&b)) return approx(*x, *y);
    if (const math::Vec3* x = std::get_if<math::Vec3>(&a))
        if (const math::Vec3* y = std::get_if<math::Vec3>(&b)) return math::length(*x - *y) <= 1e-4f;
    return a == b;
}

bool compare(const Node& n, const Blackboard& bb) {
    const Value* v = bb.find(n.key);
    const bool set = v && !std::holds_alternative<std::monostate>(*v);
    switch (n.op) {
    case CompareOp::IsSet: return set;
    case CompareOp::NotSet: return !set;
    case CompareOp::True: return set && truthy(*v);
    case CompareOp::False: return !set || !truthy(*v);
    case CompareOp::Equal: return set ? equal(*v, n.value) : std::holds_alternative<std::monostate>(n.value);
    case CompareOp::NotEqual: return !(set ? equal(*v, n.value) : std::holds_alternative<std::monostate>(n.value));
    default: break;
    }
    const double* x = set ? std::get_if<double>(v) : nullptr;
    const double* y = std::get_if<double>(&n.value);
    if (!x || !y) return false;
    switch (n.op) {
    case CompareOp::Less: return *x < *y;
    case CompareOp::LessEqual: return *x <= *y;
    case CompareOp::Greater: return *x > *y;
    case CompareOp::GreaterEqual: return *x >= *y;
    default: return false;
    }
}

std::string label(const Node& n) {
    if (!n.name.empty()) return n.name;
    std::string out(node_type_name(n.type));
    if (!n.action.empty() && (n.type == NodeType::Action || n.type == NodeType::Log)) out += " " + n.action;
    else if (!n.key.empty()) out += " " + n.key;
    if (n.type == NodeType::Wait || n.type == NodeType::Cooldown || n.type == NodeType::Timeout)
        out += std::format(" {:g}s", n.seconds);
    return out;
}

} // namespace

struct AiSystem::Impl {
    TreeLoader loader;
    std::unordered_map<std::string, CachedTree> cache;
    std::unordered_map<std::string, ActionFn> actions;
    ActionFn fallback; // actions with no registered function (Luau script tasks)
    std::unordered_map<Entity, Instance, ecs::EntityHash> instances;
    std::unordered_map<Entity, Blackboard, ecs::EntityHash> blackboards;
    std::unordered_map<Entity, PerceptionState, ecs::EntityHash> perception;
    std::vector<Noise> noises;
    std::unordered_set<std::string> warned_actions;
    const Scene* scene{nullptr};

    // Per-tick context.
    Scene* s{nullptr};
    Entity e;
    Instance* in{nullptr};
    Blackboard* bb{nullptr};
    float dt{0};

    const CachedTree& load(const std::string& path) {
        if (auto it = cache.find(path); it != cache.end()) return it->second;
        CachedTree c;
        if (path.empty()) c.error = "no behaviour tree set";
        else if (!loader) c.error = "no tree loader for " + path;
        else if (auto text = loader(path); !text) c.error = path + ": " + text.error().message;
        else if (auto tree = BehaviorTree::parse(*text, path); !tree) c.error = path + ": " + tree.error().message;
        else c.tree = *tree;
        if (!c.error.empty()) VICTUS_LOG_WARN("ai", "{}", c.error);
        return cache.emplace(path, std::move(c)).first->second;
    }

    void start(Instance& inst, std::shared_ptr<const BehaviorTree> tree, Entity entity) {
        if (!inst.seeded && s) {
            // Per-entity random stream (stable across runs for a saved scene).
            const Uuid id = s->uuid(entity);
            inst.rng.reseed(id.hi ^ (id.lo * 0x9e3779b97f4a7c15ull) ^ entity.index);
            inst.seeded = true;
        }
        inst.tree = std::move(tree);
        inst.st.assign(inst.tree ? inst.tree->nodes().size() : 0, {});
        inst.acc = 0;
        inst.last_root = NodeStatus::Idle;
        inst.ticks = 0;
        inst.completions = 0;
        if (inst.tree) {
            Blackboard& b = blackboards[entity];
            for (const auto& [k, v] : inst.tree->defaults().entries())
                if (!b.has(k)) b.set(k, v);
        }
    }

    // ---- node execution -------------------------------------------------------------------

    const std::vector<Node>& nodes() const { return in->tree->nodes(); }

    std::uint32_t subtree_end(std::uint32_t i) const {
        const auto& n = nodes();
        std::uint32_t j = i + 1;
        while (j < n.size() && n[j].depth > n[i].depth) ++j;
        return j;
    }

    void abort_node(std::uint32_t j) {
        NodeState& st = in->st[j];
        if (!st.running) return;
        st.running = false;
        st.last = NodeStatus::Idle;
        const Node& n = nodes()[j];
        if (n.type == NodeType::MoveTo && st.dest_set) {
            if (auto* nav = navigation::navigation_of(*s)) {
                const auto a = nav->agent_state(e);
                if (a.has_destination && math::length(a.destination - st.dest) < 1e-3f) nav->stop(*s, e);
            }
        } else if (n.type == NodeType::Action) {
            ActionContext ctx{*s, e, *bb, n, dt, false, true};
            if (auto it = actions.find(n.action); it != actions.end() && it->second) (void)it->second(ctx);
            else if (fallback) (void)fallback(ctx);
        }
    }

    void abort_subtree(std::uint32_t i) {
        for (std::uint32_t j = i, end = subtree_end(i); j < end; ++j) abort_node(j);
    }

    NodeStatus tick(std::uint32_t i) {
        NodeState& st = in->st[i];
        const bool first = !st.running;
        if (first) {
            st.child = 0;
            st.counter = 0;
            st.start = in->time - dt; // the frame the node starts in counts towards its time
            st.dest_set = false;
            if (nodes()[i].type == NodeType::Parallel)
                for (std::uint32_t c : nodes()[i].children) in->st[c].done = false;
        }
        st.visit = in->tick_id;
        const NodeStatus out = exec(i, first);
        NodeState& after = in->st[i]; // same storage (vector is not resized during ticks)
        after.running = out == NodeStatus::Running;
        after.last = out;
        return out;
    }

    NodeStatus exec(std::uint32_t i, bool first) {
        const Node& n = nodes()[i];
        NodeState& st = in->st[i];
        switch (n.type) {
        case NodeType::Sequence:
        case NodeType::Selector: {
            const bool seq = n.type == NodeType::Sequence;
            const NodeStatus stop_on = seq ? NodeStatus::Failure : NodeStatus::Success;
            for (std::uint32_t c = n.reactive ? 0 : st.child; c < n.children.size(); ++c) {
                const NodeStatus r = tick(n.children[c]);
                if (r == NodeStatus::Running) {
                    st.child = c;
                    return NodeStatus::Running;
                }
                if (r == stop_on) return r;
            }
            return seq ? NodeStatus::Success : NodeStatus::Failure;
        }
        case NodeType::Parallel: {
            const auto total = static_cast<std::uint32_t>(n.children.size());
            const std::uint32_t need = n.threshold ? n.threshold : total;
            std::uint32_t ok = 0, bad = 0;
            for (std::uint32_t c : n.children) {
                NodeState& cs = in->st[c];
                if (!cs.done) {
                    const NodeStatus r = tick(c);
                    if (r != NodeStatus::Running) in->st[c].done = true;
                }
                if (in->st[c].done) (in->st[c].last == NodeStatus::Success ? ok : bad) += 1;
            }
            NodeStatus out = NodeStatus::Running;
            if (ok >= need) out = NodeStatus::Success;
            else if (bad > total - need) out = NodeStatus::Failure;
            if (out != NodeStatus::Running)
                for (std::uint32_t c : n.children) abort_subtree(c);
            return out;
        }
        case NodeType::Inverter: {
            const NodeStatus r = tick(n.children[0]);
            return r == NodeStatus::Success ? NodeStatus::Failure : r == NodeStatus::Failure ? NodeStatus::Success : r;
        }
        case NodeType::Succeeder: return tick(n.children[0]) == NodeStatus::Running ? NodeStatus::Running : NodeStatus::Success;
        case NodeType::Failer: return tick(n.children[0]) == NodeStatus::Running ? NodeStatus::Running : NodeStatus::Failure;
        case NodeType::Repeat: {
            const NodeStatus r = tick(n.children[0]);
            if (r != NodeStatus::Success) return r;
            ++st.counter;
            return n.count && st.counter >= n.count ? NodeStatus::Success : NodeStatus::Running;
        }
        case NodeType::Cooldown: {
            if (first && in->time + 1e-6 < st.cooldown_until) return NodeStatus::Failure;
            const NodeStatus r = tick(n.children[0]);
            if (r != NodeStatus::Running) in->st[i].cooldown_until = in->time + n.seconds;
            return r;
        }
        case NodeType::Timeout: {
            if (!first && in->time - st.start + 1e-6 >= static_cast<double>(n.seconds)) {
                abort_subtree(n.children[0]);
                return NodeStatus::Failure;
            }
            return tick(n.children[0]);
        }
        case NodeType::Guard: return compare(n, *bb) ? tick(n.children[0]) : NodeStatus::Failure;
        case NodeType::Wait: {
            if (first) {
                const float dev = n.deviation > 0 ? in->rng.range_float(-n.deviation, n.deviation) : 0.0f;
                st.until = st.start + std::max(0.0f, n.seconds + dev);
            }
            return in->time + 1e-6 >= st.until ? NodeStatus::Success : NodeStatus::Running;
        }
        case NodeType::Check: return compare(n, *bb) ? NodeStatus::Success : NodeStatus::Failure;
        case NodeType::Set: bb->set(n.key, n.value); return NodeStatus::Success;
        case NodeType::Clear: bb->erase(n.key); return NodeStatus::Success;
        case NodeType::MoveTo: return move_to(n, st, first);
        case NodeType::RandomPoint: return random_point(n);
        case NodeType::Action: {
            ActionContext ctx{*s, e, *bb, n, dt, first, false};
            const auto it = actions.find(n.action);
            NodeStatus r = NodeStatus::Idle;
            if (it != actions.end() && it->second) r = it->second(ctx);
            else if (fallback) r = fallback(ctx);
            if (r == NodeStatus::Idle && (it == actions.end() || !it->second)) {
                if (warned_actions.insert(n.action).second) VICTUS_LOG_WARN("ai", "unknown action \"{}\"", n.action);
            }
            return r == NodeStatus::Idle ? NodeStatus::Failure : r;
        }
        case NodeType::Succeed: return NodeStatus::Success;
        case NodeType::Fail: return NodeStatus::Failure;
        case NodeType::Log: VICTUS_LOG_INFO("ai", "{}", n.action); return NodeStatus::Success;
        }
        return NodeStatus::Failure;
    }

    std::optional<math::Vec3> resolve(const Node& n) const {
        const Value* v = bb->find(n.key);
        if (!v) return std::nullopt;
        if (const math::Vec3* p = std::get_if<math::Vec3>(v)) return *p;
        if (const Entity* t = std::get_if<Entity>(v)) {
            if (!t->is_null() && s->world().alive(*t) && *t != e) return world_position(*s, *t);
        }
        return std::nullopt;
    }

    NodeStatus move_to(const Node& n, NodeState& st, bool first) {
        const auto target = resolve(n);
        if (!target || !s->world().alive(e)) return NodeStatus::Failure;
        const math::Vec3 pos = world_position(*s, e);
        const float dist = std::hypot(target->x - pos.x, target->z - pos.z);
        auto* nav = navigation::navigation_of(*s);
        const bool agent = nav && s->world().has<navigation::NavAgentComponent>(e);
        if (dist <= n.acceptance && (agent || std::fabs(target->y - pos.y) <= std::max(n.acceptance, 0.5f))) {
            if (agent && st.dest_set) nav->stop(*s, e);
            st.dest_set = false;
            return NodeStatus::Success;
        }
        if (agent) {
            const float repath = std::max(0.25f, n.acceptance * 0.5f);
            if (first || !st.dest_set || math::length(*target - st.dest) > repath) {
                if (!nav->set_destination(*s, e, *target)) return NodeStatus::Failure;
                st.dest = *target;
                st.dest_set = true;
                return NodeStatus::Running;
            }
            const auto a = nav->agent_state(e);
            if (!a.active) return NodeStatus::Failure;
            if (a.reached || !a.has_destination) {
                st.dest_set = false;
                // Arrived as close as the navmesh allows: success unless the goal was unreachable.
                return a.partial ? NodeStatus::Failure : NodeStatus::Success;
            }
            return NodeStatus::Running;
        }
        // No agent: move the transform straight towards the target.
        auto* tr = s->world().get<scene::Transform>(e);
        if (!tr) return NodeStatus::Failure;
        st.dest = *target;
        st.dest_set = true;
        const math::Vec3 to = *target - pos;
        const float len = math::length(to);
        const float step = n.speed * dt;
        const math::Vec3 delta = len <= step || len <= 1e-6f ? to : to * (step / len);
        // Positions are parent-relative: convert the world delta.
        const scene::Entity parent = s->parent(e);
        const math::Vec3 local = parent.is_null() ? delta : math::inverse(s->world_matrix(parent)).transform_vector(delta);
        tr->position = tr->position + local;
        if (len > 1e-4f && (std::fabs(to.x) + std::fabs(to.z)) > 1e-4f)
            tr->rotation = math::Quat::axis_angle({0, 1, 0}, std::atan2(to.x, to.z));
        return len <= step ? NodeStatus::Success : NodeStatus::Running;
    }

    NodeStatus random_point(const Node& n) {
        if (!s->world().alive(e)) return NodeStatus::Failure;
        const math::Vec3 origin = world_position(*s, e);
        auto* nav = navigation::navigation_of(*s);
        if (nav) (void)nav->ensure_built_async(*s);
        for (int attempt = 0; attempt < 8; ++attempt) {
            const float a = in->rng.range_float(0.0f, 2.0f * std::numbers::pi_v<float>);
            const float r = n.radius * std::sqrt(in->rng.next_float());
            const math::Vec3 p{origin.x + std::sin(a) * r, origin.y, origin.z + std::cos(a) * r};
            if (!nav || !nav->valid()) {
                bb->set(n.key, p);
                return NodeStatus::Success;
            }
            if (auto q = nav->nearest(p, {1.0f, 4.0f, 1.0f})) {
                bb->set(n.key, *q);
                return NodeStatus::Success;
            }
        }
        return NodeStatus::Failure;
    }

    void run(Instance& inst, float step) {
        in = &inst;
        bb = &blackboards[e];
        dt = step;
        inst.time += step;
        ++inst.tick_id;
        ++inst.ticks;
        const NodeStatus r = tick(0);
        // Running nodes that were not reached this tick were pre-empted.
        for (std::uint32_t j = 0; j < inst.st.size(); ++j)
            if (inst.st[j].running && inst.st[j].visit != inst.tick_id) abort_node(j);
        inst.last_root = r;
        if (r != NodeStatus::Running) ++inst.completions;
        in = nullptr;
    }

    void abort_all(Instance& inst) {
        if (!inst.tree) return;
        in = &inst;
        bb = &blackboards[e];
        for (std::uint32_t j = 0; j < inst.st.size(); ++j) abort_node(j);
        in = nullptr;
    }

    // ---- perception -----------------------------------------------------------------------

    void perceive(Scene& sc, float step, AiStats& stats) {
        auto& w = sc.world();
        struct Target {
            Entity e;
            std::uint32_t team;
            math::Vec3 aim;
        };
        std::vector<Target> targets;
        w.each<PerceptibleComponent>([&](Entity t, PerceptibleComponent& p) {
            if (p.enabled && p.team) targets.push_back({t, p.team, world_position(sc, t) + math::Vec3{0, p.height, 0}});
        });
        std::vector<std::pair<Entity, PerceptionComponent>> perceivers;
        w.each<PerceptionComponent>([&](Entity e2, PerceptionComponent& p) { perceivers.emplace_back(e2, p); });
        stats.perceivers = static_cast<std::uint32_t>(perceivers.size());
        const physics::PhysicsWorld* phys = physics::world_of(sc);
        std::unordered_set<Entity, ecs::EntityHash> live;
        for (const auto& [pe, pc] : perceivers) {
            live.insert(pe);
            PerceptionState& ps = perception[pe];
            const math::Mat4 m = sc.world_matrix(pe);
            const math::Vec3 pos = m.transform_point({});
            const math::Vec3 eye = pos + math::Vec3{0, pc.eye_height, 0};
            math::Vec3 fwd = m.transform_vector({0, 0, 1});
            fwd = math::length(fwd) > 1e-6f ? math::normalize(fwd) : math::Vec3{0, 0, 1};
            const float half = std::clamp(std::isfinite(pc.fov) ? pc.fov : 120.0f, 0.0f, 360.0f) * 0.5f;
            const float cos_half = std::cos(math::radians(std::min(half, 180.0f)));
            const float range = std::isfinite(pc.sight_range) ? std::max(pc.sight_range, 0.0f) : 0.0f;
            std::vector<std::pair<float, Entity>> seen;
            for (const Target& t : targets) {
                if (t.e == pe || !(t.team & pc.target_mask) || sc.is_descendant(t.e, pe)) continue;
                const math::Vec3 d = t.aim - eye;
                const float dist = math::length(d);
                if (dist > range) continue;
                if (dist > 1e-4f && half < 180.0f && math::dot(d / dist, fwd) < cos_half) continue;
                if (pc.line_of_sight && phys && dist > 1e-4f) {
                    const auto hit = phys->raycast(eye, d / dist, dist, pc.occlusion_mask, pe);
                    if (hit && hit->entity != t.e && !sc.is_descendant(hit->entity, t.e) && hit->distance < dist - 0.05f) continue;
                }
                seen.emplace_back(dist, t.e);
            }
            std::sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second.bits() < b.second.bits();
            });
            ps.seen.clear();
            for (const auto& [d, t] : seen) ps.seen.push_back(t);
            const bool keep = !ps.target.is_null() && std::find(ps.seen.begin(), ps.seen.end(), ps.target) != ps.seen.end();
            if (!keep && !ps.seen.empty()) ps.target = ps.seen.front();
            ps.visible = !ps.target.is_null() && std::find(ps.seen.begin(), ps.seen.end(), ps.target) != ps.seen.end();
            if (ps.visible) {
                ps.last_position = world_position(sc, ps.target);
                ps.since_seen = 0;
            } else if (!ps.target.is_null()) {
                ps.since_seen += step;
                if (ps.since_seen > pc.memory || !w.alive(ps.target)) ps.target = {};
            }
            bool heard_now = false;
            for (const Noise& nz : noises) {
                if (nz.source == pe) continue;
                if (math::length(nz.position - pos) <= std::max(pc.hearing_range, 0.0f) + std::max(nz.loudness, 0.0f) &&
                    pc.hearing_range > 0) {
                    ps.noise_position = nz.position;
                    heard_now = true;
                }
            }
            if (heard_now) {
                ps.heard = true;
                ps.since_heard = 0;
            } else if (ps.heard) {
                ps.since_heard += step;
                if (ps.since_heard > pc.memory) ps.heard = false;
            }
            Blackboard& b = blackboards[pe];
            const std::string& key = pc.target_key.empty() ? std::string("target") : pc.target_key;
            if (!ps.target.is_null()) {
                b.set(key, ps.target);
                b.set(key + "_position", ps.last_position);
            } else {
                b.erase(key);
            }
            b.set(key + "_visible", ps.visible);
            b.set("heard", ps.heard);
            if (ps.heard) b.set("noise_position", ps.noise_position);
        }
        noises.clear();
        std::erase_if(perception, [&](const auto& kv) { return !live.contains(kv.first); });
    }
};

AiSystem::AiSystem(TreeLoader loader) : impl_(std::make_unique<Impl>()) {
    register_components();
    impl_->loader = std::move(loader);
}

AiSystem::~AiSystem() = default;

void AiSystem::register_action(std::string name, ActionFn fn) { impl_->actions[std::move(name)] = std::move(fn); }

void AiSystem::set_fallback_action(ActionFn fn) { impl_->fallback = std::move(fn); }

bool AiSystem::has_fallback_action() const noexcept { return static_cast<bool>(impl_->fallback); }

bool AiSystem::has_action(std::string_view name) const noexcept {
    return impl_->actions.contains(std::string(name));
}

void AiSystem::set_tree(Scene& scene, Entity e, std::shared_ptr<const BehaviorTree> tree) {
    Impl& im = *impl_;
    if (!scene.world().alive(e)) return;
    if (im.scene != &scene) {
        im.instances.clear();
        im.blackboards.clear();
        im.perception.clear();
        im.scene = &scene;
    }
    Instance& inst = im.instances[e];
    im.s = &scene;
    im.e = e;
    im.abort_all(inst);
    inst.override_tree = tree != nullptr;
    inst.path.clear();
    inst.error.clear();
    im.start(inst, std::move(tree), e);
}

void AiSystem::reload(std::string_view path) {
    Impl& im = *impl_;
    if (path.empty()) im.cache.clear();
    else im.cache.erase(std::string(path));
    for (auto& [e, inst] : im.instances)
        if (!inst.override_tree && (path.empty() || inst.path == path)) {
            inst.path.clear(); // re-resolved (and restarted) on the next update
            inst.error.clear();
        }
}

Blackboard* AiSystem::blackboard(Scene& scene, Entity e) {
    if (!scene.world().alive(e)) return nullptr;
    Impl& im = *impl_;
    if (im.scene != &scene) {
        im.instances.clear();
        im.blackboards.clear();
        im.perception.clear();
        im.scene = &scene;
    }
    return &im.blackboards[e];
}

const PerceptionState* AiSystem::perception(Scene& scene, Entity e) const {
    if (impl_->scene != &scene) return nullptr;
    const auto it = impl_->perception.find(e);
    return it == impl_->perception.end() ? nullptr : &it->second;
}

void AiSystem::report_noise(math::Vec3 position, float loudness, Entity source) {
    impl_->noises.push_back({position, std::isfinite(loudness) ? loudness : 0.0f, source});
}

void AiSystem::update(Scene& scene, float dt) {
    Impl& im = *impl_;
    if (!std::isfinite(dt) || dt < 0) dt = 0;
    if (im.scene != &scene) {
        im.instances.clear();
        im.blackboards.clear();
        im.perception.clear();
        im.scene = &scene;
    }
    im.s = &scene;
    im.perceive(scene, dt, stats_);

    auto& w = scene.world();
    std::vector<std::pair<Entity, BehaviorTreeComponent>> trees;
    w.each<BehaviorTreeComponent>([&](Entity e, BehaviorTreeComponent& c) { trees.emplace_back(e, c); });
    for (auto& [e, inst] : im.instances) inst.seen = false;
    stats_.trees = 0;
    stats_.lod_skipped = 0;
    for (const auto& [e, c] : trees) {
        Instance& inst = im.instances[e];
        inst.seen = true;
        im.e = e;
        if (!c.enabled) {
            im.abort_all(inst);
            continue;
        }
        if (!inst.override_tree && (inst.path != c.tree || (!inst.tree && inst.error.empty()))) {
            im.abort_all(inst);
            inst.path = c.tree;
            const CachedTree& ct = im.load(c.tree);
            inst.error = ct.error;
            if (!ct.error.empty()) ++stats_.load_errors;
            im.start(inst, ct.tree, e);
        }
        if (!inst.tree) continue;
        ++stats_.trees;
        // Simulation LOD (ADR-0165): far agents tick on their due frames with the summed delta.
        const gameplay::LodTick lod = gameplay::simulation_lod_tick(w.get<gameplay::SimulationLodComponent>(e), dt);
        if (!lod.due) {
            ++stats_.lod_skipped;
            continue;
        }
        inst.acc += lod.dt;
        const float interval = std::isfinite(c.tick_interval) ? std::max(c.tick_interval, 0.0f) : 0.0f;
        if (interval > 0 && inst.acc + 1e-6f < interval) continue;
        const float step = inst.acc;
        inst.acc = 0;
        im.run(inst, step);
        ++stats_.ticks;
    }
    // Instances whose component went away (or whose entity died) stop their running nodes.
    for (auto it = im.instances.begin(); it != im.instances.end();) {
        if (it->second.seen || it->second.override_tree) {
            if (!it->second.seen && it->second.override_tree && w.alive(it->first) && it->second.tree) {
                im.e = it->first;
                Instance& inst = it->second;
                ++stats_.trees;
                const gameplay::LodTick lod =
                    gameplay::simulation_lod_tick(w.get<gameplay::SimulationLodComponent>(it->first), dt);
                if (lod.due) {
                    inst.acc = 0;
                    im.run(inst, lod.dt);
                    ++stats_.ticks;
                } else {
                    ++stats_.lod_skipped;
                }
            }
            if (!w.alive(it->first)) {
                it = im.instances.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        if (w.alive(it->first)) {
            im.e = it->first;
            im.abort_all(it->second);
        }
        it = im.instances.erase(it);
    }
    std::erase_if(im.blackboards, [&](const auto& kv) { return !w.alive(kv.first); });
    im.s = nullptr;
}

std::optional<TreeDebug> AiSystem::debug(Scene& scene, Entity e) const {
    const Impl& im = *impl_;
    if (im.scene != &scene) return std::nullopt;
    const auto it = im.instances.find(e);
    if (it == im.instances.end()) return std::nullopt;
    const Instance& inst = it->second;
    TreeDebug d;
    d.tree = inst.override_tree ? (inst.tree ? inst.tree->name() : std::string()) : inst.path;
    d.error = inst.error;
    d.ticks = inst.ticks;
    d.completions = inst.completions;
    d.last_root = inst.last_root;
    if (inst.tree) {
        const auto& nodes = inst.tree->nodes();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const NodeState& st = inst.st[i];
            const bool active = st.visit == inst.tick_id && inst.tick_id != 0;
            d.nodes.push_back({nodes[i].type, label(nodes[i]), nodes[i].depth, active ? st.last : NodeStatus::Idle, active});
        }
    }
    return d;
}

void AiSystem::debug_lines(Scene& scene, std::vector<DebugLine>& out) const {
    const Impl& im = *impl_;
    constexpr math::Color kCone{1.0f, 0.85f, 0.2f, 1}, kSeen{1.0f, 0.2f, 0.15f, 1}, kLost{1.0f, 0.55f, 0.1f, 1},
        kHeard{0.3f, 0.7f, 1.0f, 1};
    scene.world().each<PerceptionComponent>([&](Entity e, PerceptionComponent& pc) {
        const math::Mat4 m = scene.world_matrix(e);
        const math::Vec3 eye = m.transform_point({}) + math::Vec3{0, pc.eye_height, 0};
        math::Vec3 f = m.transform_vector({0, 0, 1});
        const float yaw = std::atan2(f.x, f.z);
        const float half = math::radians(std::clamp(pc.fov, 0.0f, 360.0f) * 0.5f);
        const float r = std::max(pc.sight_range, 0.0f);
        constexpr int kSegments = 16;
        math::Vec3 prev{};
        for (int i = 0; i <= kSegments; ++i) {
            const float a = yaw - half + 2.0f * half * static_cast<float>(i) / kSegments;
            const math::Vec3 p = eye + math::Vec3{std::sin(a) * r, 0, std::cos(a) * r};
            if (i == 0 || (i == kSegments && half < std::numbers::pi_v<float>)) out.push_back({eye, p, kCone});
            if (i > 0) out.push_back({prev, p, kCone});
            prev = p;
        }
        if (im.scene != &scene) return;
        const auto it = im.perception.find(e);
        if (it == im.perception.end()) return;
        const PerceptionState& ps = it->second;
        if (!ps.target.is_null() && scene.world().alive(ps.target))
            out.push_back({eye, ps.visible ? scene.world_matrix(ps.target).transform_point({}) : ps.last_position,
                           ps.visible ? kSeen : kLost});
        if (ps.heard) out.push_back({eye, ps.noise_position, kHeard});
    });
}

// ---- registration and binding ---------------------------------------------------------------------

TreeLoader directory_loader(std::filesystem::path assets_root) {
    return [root = std::move(assets_root)](std::string_view path) -> Result<std::string> {
        auto clean = fs::sanitize_relative(path);
        if (!clean) return std::move(clean).error();
        return fs::read_text(root / *clean, 16u * 1024u * 1024u);
    };
}

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<BehaviorTreeComponent>("BehaviorTree", "AI")
            .field(&BehaviorTreeComponent::tree, "tree", {.tooltip = "Behaviour tree asset (.vbt) relative to the assets folder", .hint = "behavior_tree"})
            .field(&BehaviorTreeComponent::tick_interval, "tick_interval", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "Seconds between ticks (0 = every frame)"})
            .field(&BehaviorTreeComponent::enabled, "enabled")
            .commit();
        scene::Schema<PerceptionComponent>("Perception", "AI")
            .field(&PerceptionComponent::sight_range, "sight_range", {.min = 0, .max = 1000, .speed = 0.1f, .tooltip = "m"})
            .field(&PerceptionComponent::fov, "fov", {.min = 0, .max = 360, .speed = 0.5f, .tooltip = "Full sight cone angle around +Z", .hint = "degrees"})
            .field(&PerceptionComponent::eye_height, "eye_height", {.min = -100, .max = 100, .speed = 0.01f})
            .field(&PerceptionComponent::hearing_range, "hearing_range", {.min = 0, .max = 1000, .speed = 0.1f, .tooltip = "m (0 = deaf)"})
            .field(&PerceptionComponent::memory, "memory", {.min = 0, .max = 600, .speed = 0.05f, .tooltip = "Seconds a lost target or a noise is remembered"})
            .field(&PerceptionComponent::target_mask, "target_mask", {.tooltip = "Perceptible teams that are noticed", .hint = "bitmask16"})
            .field(&PerceptionComponent::occlusion_mask, "occlusion_mask", {.tooltip = "Collider layers that block sight", .hint = "bitmask16"})
            .field(&PerceptionComponent::line_of_sight, "line_of_sight", {.tooltip = "Physics ray must reach the target"})
            .field(&PerceptionComponent::target_key, "target_key", {.tooltip = "Blackboard key for the current target"})
            .commit();
        scene::Schema<PerceptibleComponent>("Perceptible", "AI")
            .field(&PerceptibleComponent::team, "team", {.tooltip = "Team bits matched against Perception target_mask", .hint = "bitmask16"})
            .field(&PerceptibleComponent::height, "height", {.min = -100, .max = 100, .speed = 0.01f, .tooltip = "Aim point above the origin"})
            .field(&PerceptibleComponent::enabled, "enabled")
            .commit();
    });
}

namespace {
struct Binding {
    AiSystem* ai{nullptr};
};
} // namespace

void bind(Scene& scene, AiSystem* ai) {
    if (auto* b = scene.world().resource<Binding>()) b->ai = ai;
    else scene.world().set_resource<Binding>().ai = ai;
}

AiSystem* ai_of(Scene& scene) {
    const auto* b = scene.world().resource<Binding>();
    return b ? b->ai : nullptr;
}

void add_ai_systems(gameplay::Simulation& sim, AiSystem& ai) {
    register_components();
    AiSystem* a = &ai;
    sim.add_system("ai.bind", gameplay::Phase::PreUpdate, [a](Scene& scene, const gameplay::FrameTime&) {
        if (ai_of(scene) != a) bind(scene, a);
    });
    sim.add_system("ai.update", gameplay::Phase::Update, [a](Scene& scene, const gameplay::FrameTime& t) {
        if (ai_of(scene) != a) bind(scene, a);
        a->update(scene, t.dt);
    });
}

} // namespace victus::ai
