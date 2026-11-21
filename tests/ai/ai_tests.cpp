// SPDX-License-Identifier: Apache-2.0
// Gameplay AI tests (Slice 73): blackboard, behaviour tree parsing and execution semantics,
// navigation-driven move_to, perception (sight cone, teams, occlusion, memory, hearing) and the
// BehaviorTree component with a tree loader.
#include "victus/ai/ai.hpp"
#include "victus/gameplay/sim_lod.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <cmath>
#include <map>

using namespace victus;
using namespace victus::ai;
using scene::Scene;
using scene::Transform;

namespace {

std::shared_ptr<const BehaviorTree> tree(std::string_view text) {
    auto t = BehaviorTree::parse(text, "test");
    if (!t) FAIL_CHECK(t.error().message);
    REQUIRE(t.ok());
    return *t;
}

Entity actor(Scene& s, math::Vec3 p = {}, const char* name = "Actor") {
    const Entity e = s.create(name);
    s.world().get<Transform>(e)->position = p;
    return e;
}

Entity box(Scene& s, math::Vec3 center, math::Vec3 size) {
    const Entity e = s.create("Box");
    s.world().get<Transform>(e)->position = center;
    physics::ColliderComponent c;
    c.size = size;
    s.world().add<physics::ColliderComponent>(e, c);
    return e;
}

// Counts calls per action name and returns the scripted status.
struct Recorder {
    std::map<std::string, int> calls, firsts, aborts;
    std::map<std::string, NodeStatus> result;
    void install(AiSystem& ai, const std::string& name) {
        result[name] = NodeStatus::Success;
        ai.register_action(name, [this, name](ActionContext& c) {
            if (c.aborted) {
                ++aborts[name];
                return NodeStatus::Idle;
            }
            ++calls[name];
            if (c.first) ++firsts[name];
            return result[name];
        });
    }
};

} // namespace

TEST_CASE("ai: blackboard values, comparisons and tree validation") {
    Blackboard bb;
    CHECK_FALSE(bb.has("hp"));
    bb.set("hp", 10.0);
    bb.set("alert", true);
    bb.set("home", math::Vec3{1, 2, 3});
    const std::uint64_t rev = bb.revision();
    bb.set("hp", 10.0); // unchanged value: no revision bump
    CHECK(bb.revision() == rev);
    CHECK(bb.number("hp") == 10.0);
    CHECK(bb.boolean("alert"));
    CHECK(bb.vec3("home")->y == 2.0f);
    CHECK(bb.entity("hp").is_null());
    CHECK(bb.entries().front().first == "alert"); // sorted keys
    CHECK(bb.erase("alert"));
    CHECK_FALSE(bb.erase("alert"));
    CHECK(to_string(Value{math::Vec3{1, 2, 3}}) == "(1.00, 2.00, 3.00)");
    CHECK(std::holds_alternative<math::Vec3>(value_from_json(*json::parse("[1, 2, 3]"))));
    CHECK(std::holds_alternative<std::monostate>(value_from_json(*json::parse("{}"))));

    auto ok = tree(R"({"blackboard": {"speed": 3, "mode": "patrol"},
        "root": {"type": "sequence", "children": [
            {"type": "check", "key": "speed", "op": ">=", "value": 3},
            {"type": "inverter", "child": {"type": "check", "key": "mode", "value": "chase"}},
            {"type": "wait", "seconds": 1}]}})");
    CHECK(ok->nodes().size() == 5);
    CHECK(ok->nodes()[0].children.size() == 3);
    CHECK(ok->nodes()[3].depth == 2);
    CHECK(ok->nodes()[1].op == CompareOp::GreaterEqual);
    CHECK(ok->nodes()[2].type == NodeType::Inverter);
    CHECK(ok->defaults().number("speed") == 3.0);

    for (const char* bad : {
             R"({"root": {"type": "dance"}})",
             R"({"root": {"type": "check"}})",
             R"({"root": {"type": "inverter"}})",
             R"({"root": {"type": "inverter", "children": [{"type": "succeed"}, {"type": "fail"}]}})",
             R"({"root": {"type": "sequence", "children": []}})",
             R"({"root": {"type": "wait", "child": {"type": "succeed"}}})",
             R"({"root": {"type": "set", "key": "x"}})",
             R"({"root": {"type": "check", "key": "x", "op": "~="}})",
             R"({"root": {"type": "action"}})",
             R"({"tree": {}})",
             R"([1, 2])",
             R"({"root": )",
         }) {
        CAPTURE(bad);
        auto r = BehaviorTree::parse(bad);
        CHECK_FALSE(r.ok());
    }
}

TEST_CASE("ai: composites remember running children; reactive selectors and guards pre-empt") {
    Scene s;
    AiSystem ai;
    Recorder rec;
    for (const char* n : {"a", "b", "c", "attack", "patrol"}) rec.install(ai, n);
    const Entity e = actor(s);

    // Sequence with memory: a runs for two ticks, then b; a is not re-ticked once done.
    ai.set_tree(s, e, tree(R"({"root": {"type": "sequence", "children": [
        {"type": "action", "action": "a"}, {"type": "action", "action": "b"}]}})"));
    rec.result["a"] = NodeStatus::Running;
    ai.update(s, 0.1f);
    ai.update(s, 0.1f);
    CHECK(rec.calls["a"] == 2);
    CHECK(rec.firsts["a"] == 1);
    rec.result["a"] = NodeStatus::Success;
    ai.update(s, 0.1f);
    CHECK(rec.calls["a"] == 3);
    CHECK(rec.calls["b"] == 1);
    auto d = ai.debug(s, e);
    REQUIRE(d);
    CHECK(d->completions == 1);
    CHECK(d->last_root == NodeStatus::Success);
    CHECK(d->nodes.size() == 3);
    CHECK(d->nodes[1].label == "action a");

    // Reactive selector: attack pre-empts a running patrol as soon as the guard passes; patrol
    // gets an abort call.
    ai.set_tree(s, e, tree(R"({"root": {"type": "selector", "reactive": true, "children": [
        {"type": "guard", "key": "target", "child": {"type": "action", "action": "attack"}},
        {"type": "action", "action": "patrol"}]}})"));
    rec.result["patrol"] = NodeStatus::Running;
    rec.result["attack"] = NodeStatus::Running;
    ai.update(s, 0.1f);
    ai.update(s, 0.1f);
    CHECK(rec.calls["patrol"] == 2);
    CHECK(rec.calls["attack"] == 0);
    ai.blackboard(s, e)->set("target", math::Vec3{1, 0, 0});
    ai.update(s, 0.1f);
    CHECK(rec.calls["attack"] == 1);
    CHECK(rec.aborts["patrol"] == 1);
    d = ai.debug(s, e);
    CHECK(d->nodes[1].status == NodeStatus::Running); // guard
    CHECK(d->nodes[3].status == NodeStatus::Idle);    // patrol not reached
    CHECK_FALSE(d->nodes[3].active);
    // The target disappears: the guard fails, attack is aborted and patrol restarts.
    ai.blackboard(s, e)->erase("target");
    ai.update(s, 0.1f);
    CHECK(rec.aborts["attack"] == 1);
    CHECK(rec.firsts["patrol"] == 2);

    // Unknown actions fail without crashing.
    ai.set_tree(s, e, tree(R"({"root": {"type": "action", "action": "nope"}})"));
    ai.update(s, 0.1f);
    CHECK(ai.debug(s, e)->last_root == NodeStatus::Failure);
}

TEST_CASE("ai: parallel, repeat, cooldown, timeout, wait and blackboard leaves") {
    Scene s;
    AiSystem ai;
    Recorder rec;
    for (const char* n : {"x", "y", "z"}) rec.install(ai, n);
    const Entity e = actor(s);

    // Parallel needing one success: y succeeds on the second tick, x (still running) is aborted.
    ai.set_tree(s, e, tree(R"({"root": {"type": "parallel", "threshold": 1, "children": [
        {"type": "action", "action": "x"}, {"type": "action", "action": "y"}]}})"));
    rec.result["x"] = NodeStatus::Running;
    rec.result["y"] = NodeStatus::Running;
    ai.update(s, 0.1f);
    rec.result["y"] = NodeStatus::Success;
    ai.update(s, 0.1f);
    CHECK(ai.debug(s, e)->last_root == NodeStatus::Success);
    CHECK(rec.aborts["x"] == 1);

    // Repeat 3 times, one child run per tick, then succeed.
    ai.set_tree(s, e, tree(R"({"root": {"type": "repeat", "count": 3, "child": {"type": "action", "action": "z"}}})"));
    for (int i = 0; i < 3; ++i) ai.update(s, 0.1f);
    CHECK(rec.calls["z"] == 3);
    CHECK(ai.debug(s, e)->completions == 1);

    // Wait 0.5 s, then set a key; cooldown 1 s blocks re-entry.
    ai.set_tree(s, e, tree(R"({"root": {"type": "selector", "children": [
        {"type": "cooldown", "seconds": 1, "child": {"type": "sequence", "children": [
            {"type": "wait", "seconds": 0.5}, {"type": "set", "key": "count", "value": 1}]}},
        {"type": "set", "key": "blocked", "value": true}]}})"));
    Blackboard& bb = *ai.blackboard(s, e);
    for (int i = 0; i < 4; ++i) ai.update(s, 0.1f);
    CHECK_FALSE(bb.has("count"));
    ai.update(s, 0.1f); // 0.5 s elapsed
    CHECK(bb.number("count") == 1.0);
    CHECK_FALSE(bb.has("blocked"));
    ai.update(s, 0.1f); // cooling down: the selector falls through
    CHECK(bb.boolean("blocked"));
    bb.erase("count");
    for (int i = 0; i < 9; ++i) ai.update(s, 0.1f); // cooling down until 1.5 s
    CHECK_FALSE(bb.has("count"));
    for (int i = 0; i < 5; ++i) ai.update(s, 0.1f); // re-entered at 1.5 s, waited 0.5 s
    CHECK(bb.has("count"));

    // Timeout fails a long action and aborts it; clear removes keys; tick_interval batches dt.
    ai.set_tree(s, e, tree(R"({"root": {"type": "sequence", "children": [
        {"type": "clear", "key": "count"},
        {"type": "timeout", "seconds": 0.3, "child": {"type": "action", "action": "x"}}]}})"));
    const int aborts = rec.aborts["x"];
    for (int i = 0; i < 3; ++i) ai.update(s, 0.1f); // fails on the tick 0.3 s is reached
    CHECK_FALSE(bb.has("count"));
    CHECK(rec.aborts["x"] == aborts + 1);
    CHECK(ai.debug(s, e)->last_root == NodeStatus::Failure);
}

TEST_CASE("ai: move_to drives navmesh agents and plain transforms; random_point stays nearby") {
    Scene s;
    box(s, {0, -0.5f, 0}, {30, 1, 30});
    navigation::NavigationSystem nav;
    AiSystem ai;
    gameplay::Simulation sim;
    navigation::add_navigation_systems(sim, nav);
    add_ai_systems(sim, ai);

    const Entity agent = actor(s, {-6, 0, -6}, "Agent");
    s.world().add<navigation::NavAgentComponent>(agent, navigation::NavAgentComponent{});
    const Entity walker = actor(s, {0, 0, 0}, "Walker");
    const Entity goal = actor(s, {6, 0, 6}, "Goal");
    const auto chase = tree(R"({"root": {"type": "sequence", "children": [
        {"type": "move_to", "key": "goal", "acceptance": 0.6},
        {"type": "set", "key": "arrived", "value": true}]}})");
    sim.tick(s, 1.0f / 60.0f);
    ai.set_tree(s, agent, chase);
    ai.set_tree(s, walker, chase);
    ai.blackboard(s, agent)->set("goal", goal);
    ai.blackboard(s, walker)->set("goal", math::Vec3{-4, 0, 3});
    for (int i = 0; i < 60 * 10 && !(ai.blackboard(s, agent)->has("arrived") && ai.blackboard(s, walker)->has("arrived")); ++i)
        sim.tick(s, 1.0f / 60.0f);
    CHECK(ai.blackboard(s, agent)->boolean("arrived"));
    CHECK(ai.blackboard(s, walker)->boolean("arrived"));
    const math::Vec3 a = s.world().get<Transform>(agent)->position;
    CHECK(std::hypot(a.x - 6, a.z - 6) <= 0.7f);
    CHECK(math::length(s.world().get<Transform>(walker)->position - math::Vec3{-4, 0, 3}) < 0.7f);

    // A moving goal is re-pathed; an unreachable one fails.
    ai.blackboard(s, agent)->erase("arrived");
    s.world().get<Transform>(goal)->position = {-8, 0, 8};
    for (int i = 0; i < 60 * 10 && !ai.blackboard(s, agent)->has("arrived"); ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK(ai.blackboard(s, agent)->boolean("arrived"));
    ai.blackboard(s, agent)->set("goal", Entity{});
    sim.tick(s, 1.0f / 60.0f);
    CHECK(ai.debug(s, agent)->last_root == NodeStatus::Failure);

    // random_point picks reachable points within the radius, deterministically per entity.
    ai.set_tree(s, agent, tree(R"({"root": {"type": "random_point", "key": "p", "radius": 5}})"));
    sim.tick(s, 1.0f / 60.0f);
    const auto p = ai.blackboard(s, agent)->vec3("p");
    REQUIRE(p);
    const math::Vec3 at = s.world().get<Transform>(agent)->position;
    CHECK(std::hypot(p->x - at.x, p->z - at.z) <= 5.2f);
    CHECK(std::fabs(p->y) < 0.3f);
}

TEST_CASE("ai: perception sees teams in the cone, respects walls and memory, and hears noises") {
    auto world = physics::PhysicsWorld::create({});
    REQUIRE(world.ok());
    Scene s;
    AiSystem ai;
    gameplay::Simulation sim;
    physics::add_physics_systems(sim, *world.value());
    add_ai_systems(sim, ai);

    const Entity guard = actor(s, {0, 0, 0}, "Guard"); // looks along +Z
    PerceptionComponent pc;
    pc.sight_range = 10;
    pc.fov = 90;
    pc.memory = 0.5f;
    pc.hearing_range = 6;
    pc.target_mask = 0b10;
    s.world().add<PerceptionComponent>(guard, pc);
    const Entity player = actor(s, {0, 0, 5}, "Player");
    s.world().add<PerceptibleComponent>(player, PerceptibleComponent{.team = 0b10});
    const Entity ally = actor(s, {1, 0, 3}, "Ally");
    s.world().add<PerceptibleComponent>(ally, PerceptibleComponent{.team = 0b01});
    const float dt = 1.0f / 60.0f;
    sim.tick(s, dt);
    const PerceptionState* ps = ai.perception(s, guard);
    REQUIRE(ps);
    CHECK(ps->visible);
    CHECK(ps->target == player);
    CHECK(ps->seen.size() == 1); // the ally's team is not in the mask
    Blackboard& bb = *ai.blackboard(s, guard);
    CHECK(bb.entity("target") == player);
    CHECK(bb.boolean("target_visible"));
    CHECK(bb.vec3("target_position")->z == doctest::Approx(5));

    // Behind the guard: out of the cone; remembered for `memory` seconds, then forgotten.
    s.world().get<Transform>(player)->position = {0, 0, -5};
    sim.tick(s, dt);
    CHECK_FALSE(ps->visible);
    CHECK(bb.entity("target") == player);
    CHECK_FALSE(bb.boolean("target_visible"));
    CHECK(bb.vec3("target_position")->z == doctest::Approx(5)); // last known position
    for (int i = 0; i < 40; ++i) sim.tick(s, dt);
    CHECK(ps->target.is_null());
    CHECK_FALSE(bb.has("target"));

    // In front but out of range, then in range behind a wall.
    s.world().get<Transform>(player)->position = {0, 0, 12};
    sim.tick(s, dt);
    CHECK_FALSE(ps->visible);
    s.world().get<Transform>(player)->position = {0, 0, 6};
    box(s, {0, 1, 3}, {4, 4, 0.5f});
    sim.tick(s, dt);
    sim.tick(s, dt);
    CHECK_FALSE(ps->visible);
    s.world().get<PerceptionComponent>(guard)->line_of_sight = false;
    sim.tick(s, dt);
    CHECK(ps->visible);

    // Noises within the hearing range (plus loudness) are heard; the guard's own are not.
    CHECK_FALSE(bb.boolean("heard"));
    ai.report_noise({20, 0, 0}, 1.0f);
    sim.tick(s, dt);
    CHECK_FALSE(bb.boolean("heard"));
    ai.report_noise({0, 0, 0}, 0.0f, guard);
    sim.tick(s, dt);
    CHECK_FALSE(bb.boolean("heard"));
    ai.report_noise({-10, 0, 0}, 5.0f);
    sim.tick(s, dt);
    CHECK(bb.boolean("heard"));
    CHECK(bb.vec3("noise_position")->x == doctest::Approx(-10));
    for (int i = 0; i < 40; ++i) sim.tick(s, dt);
    CHECK_FALSE(bb.boolean("heard"));

    std::vector<DebugLine> lines;
    ai.debug_lines(s, lines);
    CHECK(lines.size() > 16);
}

TEST_CASE("ai: BehaviorTree components load trees through the loader, reload and report errors") {
    std::map<std::string, std::string> files{
        {"ai/idle.vbt", R"({"blackboard": {"mood": "calm"}, "root": {"type": "set", "key": "ran", "value": 1}})"},
        {"ai/broken.vbt", R"({"root": {"type": "nope"}})"},
    };
    int loads = 0;
    AiSystem ai([&](std::string_view path) -> Result<std::string> {
        ++loads;
        const auto it = files.find(std::string(path));
        if (it == files.end()) return make_error(ErrorCode::NotFound, "missing");
        return it->second;
    });
    Scene s;
    const Entity a = actor(s, {}, "A"), b = actor(s, {}, "B"), c = actor(s, {}, "C");
    s.world().add<BehaviorTreeComponent>(a, BehaviorTreeComponent{.tree = "ai/idle.vbt"});
    s.world().add<BehaviorTreeComponent>(b, BehaviorTreeComponent{.tree = "ai/idle.vbt", .tick_interval = 0.5f});
    s.world().add<BehaviorTreeComponent>(c, BehaviorTreeComponent{.tree = "ai/broken.vbt"});
    ai.update(s, 0.1f);
    CHECK(loads == 2); // cached per path
    CHECK(ai.blackboard(s, a)->number("ran") == 1.0);
    CHECK(ai.blackboard(s, a)->find("mood"));
    CHECK_FALSE(ai.blackboard(s, b)->has("ran")); // waits for its first 0.5 s interval
    CHECK(ai.stats().trees == 2);
    CHECK(ai.stats().load_errors == 1);
    auto dc = ai.debug(s, c);
    REQUIRE(dc);
    CHECK(dc->error.find("unknown node type") != std::string::npos);
    for (int i = 0; i < 4; ++i) ai.update(s, 0.1f);
    CHECK(ai.blackboard(s, b)->number("ran") == 1.0);
    CHECK(ai.debug(s, b)->ticks == 1);
    CHECK(ai.debug(s, a)->ticks == 5);

    // Fix the broken file and reload: it loads again and runs.
    files["ai/broken.vbt"] = files["ai/idle.vbt"];
    ai.reload("ai/broken.vbt");
    ai.update(s, 0.1f);
    CHECK(ai.debug(s, c)->error.empty());
    CHECK(ai.blackboard(s, c)->number("ran") == 1.0);

    // Disabling stops ticking; removing the component drops the instance.
    s.world().get<BehaviorTreeComponent>(a)->enabled = false;
    const auto ticks = ai.debug(s, a)->ticks;
    ai.update(s, 0.1f);
    CHECK(ai.debug(s, a)->ticks == ticks);
    s.world().remove<BehaviorTreeComponent>(a);
    ai.update(s, 0.1f);
    CHECK_FALSE(ai.debug(s, a));

    // Components are reflected for scenes and the inspector.
    for (const char* n : {"BehaviorTree", "Perception", "Perceptible"}) CHECK(scene::find_schema(n));
    CHECK(scene::find_schema("Perception")->field("fov")->hint == "degrees");
}

TEST_CASE("ai: simulation LOD ticks far trees less often with the same simulated time (ADR-0165)") {
    gameplay::register_components();
    Scene s;
    AiSystem ai;
    gameplay::Simulation sim;
    gameplay::add_builtin_systems(sim);
    add_ai_systems(sim, ai);
    const Entity src = actor(s, {0, 0, 0}, "Player");
    s.world().add<gameplay::SimulationLodSource>(src);
    const Entity near = actor(s, {2, 0, 0}, "Near");
    const Entity far = actor(s, {400, 0, 0}, "Far");
    for (const Entity e : {near, far}) s.world().add<gameplay::SimulationLodComponent>(e);
    const auto waiter = tree(R"({"root": {"type": "sequence", "children": [
        {"type": "wait", "seconds": 1}, {"type": "set", "key": "done", "value": true}]}})");
    sim.tick(s, 1.0f / 60.0f);
    sim.tick(s, 1.0f / 60.0f);
    ai.set_tree(s, near, waiter);
    ai.set_tree(s, far, waiter);
    std::uint32_t skipped = 0;
    int near_frames = -1, far_frames = -1;
    for (int i = 0; i < 120 && (near_frames < 0 || far_frames < 0); ++i) {
        sim.tick(s, 1.0f / 60.0f);
        skipped += ai.stats().lod_skipped;
        if (near_frames < 0 && ai.blackboard(s, near)->has("done")) near_frames = i;
        if (far_frames < 0 && ai.blackboard(s, far)->has("done")) far_frames = i;
    }
    CHECK(s.world().get<gameplay::SimulationLodComponent>(far)->tier == 3);
    CHECK(skipped > 40); // the far tree runs on one frame in eight
    REQUIRE(near_frames >= 0);
    REQUIRE(far_frames >= 0);
    CHECK(near_frames >= 58);
    CHECK(near_frames <= 62);
    // The far agent finishes on its first due frame after one second: at most one interval late.
    CHECK(far_frames >= near_frames);
    CHECK(far_frames <= near_frames + 8);
}
