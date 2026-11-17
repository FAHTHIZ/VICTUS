// SPDX-License-Identifier: Apache-2.0
// Visual scripting graphs (ADR-0058): node library, deterministic serialization, validation,
// compilation to Luau and running compiled graphs in the ScriptHost.
#include "victus/core/fs.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/script/graph.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <set>

using namespace victus;
using namespace victus::script;
using scene::Entity;
using scene::Scene;

namespace {

struct GraphDir {
    std::filesystem::path path;
    GraphDir() {
        path = std::filesystem::temp_directory_path() / ("victus_graph_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path / "scripts");
    }
    ~GraphDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    // Every write gets a later mtime, so reload_changed sees rewrites even within one timestamp
    // tick on a loaded machine.
    void write(const std::string& rel, const std::string& text) {
        REQUIRE(fs::write_text_atomic(path / rel, text).ok());
        std::filesystem::last_write_time(path / rel,
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++writes));
    }
    int writes{0};
};

struct Keys final : gameplay::InputSource {
    std::set<std::string, std::less<>> down;
    bool key_down(std::string_view k) const override { return down.contains(k); }
    bool key_pressed(std::string_view k) const override { return down.contains(k); }
    bool key_released(std::string_view) const override { return false; }
    bool mouse_down(int) const override { return false; }
    bool mouse_pressed(int) const override { return false; }
    math::Vec2 mouse_position() const override { return {}; }
    math::Vec2 mouse_delta() const override { return {}; }
};

bool has_error(const std::vector<GraphDiagnostic>& diags, std::uint32_t node, std::string_view text) {
    return std::any_of(diags.begin(), diags.end(), [&](const GraphDiagnostic& d) {
        return d.severity == GraphDiagnostic::Severity::Error && (node == 0 || d.node == node) &&
               d.message.find(text) != std::string::npos;
    });
}

// A mover: On Update -> Branch(key D held) -> Translate(self, (speed * dt, 0, 0)) -> Set count = count + 1.
ScriptGraph mover_graph() {
    ScriptGraph g;
    g.variables.push_back({"speed", PinType::Number, json::Value(2), true});
    g.variables.push_back({"count", PinType::Number, json::Value(0), false});
    const auto update = g.add_node("event.update", 0, 0);
    const auto key = g.add_node("input.key_down", 0, 120);
    g.node(key)->inputs.set("key", "D");
    const auto branch = g.add_node("flow.branch", 200, 0);
    const auto speed = g.add_node("variable.get", 0, 200);
    g.node(speed)->config.set("variable", "speed");
    const auto mul = g.add_node("math.multiply", 150, 200);
    const auto make = g.add_node("vector.make", 300, 200);
    const auto move = g.add_node("entity.translate", 400, 0);
    const auto get_count = g.add_node("variable.get", 400, 200);
    g.node(get_count)->config.set("variable", "count");
    const auto add = g.add_node("math.add", 500, 200);
    g.node(add)->inputs.set("b", 1);
    const auto set_count = g.add_node("variable.set", 600, 0);
    g.node(set_count)->config.set("variable", "count");
    REQUIRE(g.connect(update, "out", branch, "in"));
    REQUIRE(g.connect(key, "held", branch, "condition"));
    REQUIRE(g.connect(branch, "true", move, "in"));
    REQUIRE(g.connect(speed, "value", mul, "a"));
    REQUIRE(g.connect(update, "dt", mul, "b"));
    REQUIRE(g.connect(mul, "result", make, "x"));
    REQUIRE(g.connect(make, "vector", move, "delta"));
    REQUIRE(g.connect(move, "out", set_count, "in"));
    REQUIRE(g.connect(get_count, "value", add, "a"));
    REQUIRE(g.connect(add, "result", set_count, "value"));
    return g;
}

} // namespace

TEST_CASE("graph: every library node compiles to valid Luau") {
    std::set<std::string> types;
    for (const NodeDesc& d : node_library()) {
        CAPTURE(d.type);
        CHECK(types.insert(d.type).second);
        CHECK(find_node_desc(d.type) == &d);
        if (d.kind == NodeKind::Pure) CHECK(d.output_code.size() == d.outputs.size());
        ScriptGraph g;
        g.variables.push_back({"v", PinType::Number, json::Value(1), true});
        const auto start = d.kind == NodeKind::Event ? 0u : g.add_node("event.start");
        const auto n = g.add_node(d.type);
        if (d.kind == NodeKind::GetVariable || d.kind == NodeKind::SetVariable) g.node(n)->config.set("variable", "v");
        if (d.kind == NodeKind::Event) {
            const auto log = g.add_node("debug.log");
            CHECK(g.connect(n, "out", log, "in"));
            if (d.outputs.size() > 1) CHECK(g.connect(n, d.outputs[1].name, log, "message"));
        } else if (d.kind == NodeKind::Pure || d.kind == NodeKind::GetVariable) {
            const auto log = g.add_node("debug.log");
            CHECK(g.connect(start, "out", log, "in"));
            auto prev = log;
            for (const PinDesc& out : d.outputs) {
                const auto l2 = g.add_node("debug.log");
                CHECK(g.connect(n, out.name, l2, "message"));
                CHECK(g.connect(prev, "out", l2, "in"));
                prev = l2;
            }
        } else {
            CHECK(g.connect(start, "out", n, "in"));
        }
        const CompiledGraph c = compile_graph(g, {.source_name = "test.vgraph"});
        INFO(c.error_text());
        REQUIRE(c.ok());
        INFO(c.source);
        CHECK(check_syntax(c.source, "test.vgraph").ok());
    }
    CHECK(types.size() > 60);
}

TEST_CASE("graph: serialization is deterministic and round-trips") {
    ScriptGraph g = mover_graph();
    g.node(1)->comment = "entry";
    const std::string text = serialize_graph(g);
    auto parsed = parse_graph(text);
    REQUIRE(parsed.ok());
    CHECK(serialize_graph(*parsed) == text);
    // Node and link order, and the order inputs were set, do not change the file.
    ScriptGraph shuffled = g;
    std::reverse(shuffled.nodes.begin(), shuffled.nodes.end());
    std::reverse(shuffled.links.begin(), shuffled.links.end());
    CHECK(serialize_graph(shuffled) == text);
    CHECK(compile_graph(*parsed).source == compile_graph(g).source);
    CHECK(parsed->next_id == g.next_id);
    CHECK(parsed->node(1)->comment == "entry");
    CHECK(parsed->variable("count")->exposed == false);
    // Wrong documents are rejected.
    CHECK_FALSE(parse_graph("{\"format\":\"other\"}").ok());
    CHECK_FALSE(parse_graph("{\"format\":\"victus.graph\",\"version\":99}").ok());
    CHECK_FALSE(parse_graph("{\"format\":\"victus.graph\",\"version\":1,\"nodes\":[{\"type\":\"x\"}]}").ok());
    CHECK_FALSE(parse_graph("not json").ok());
}

TEST_CASE("graph: editing keeps one link per data input and exec output") {
    ScriptGraph g;
    const auto start = g.add_node("event.start");
    const auto a = g.add_node("debug.log");
    const auto b = g.add_node("debug.log");
    const auto num = g.add_node("const.number");
    const auto text = g.add_node("const.string");
    CHECK(g.connect(start, "out", a, "in"));
    CHECK(g.connect(start, "out", b, "in")); // replaces the first exec link
    CHECK(g.links.size() == 1);
    CHECK(g.connect(num, "value", a, "message"));
    CHECK(g.connect(text, "value", a, "message")); // replaces
    CHECK(g.links.size() == 2);
    const auto pos = g.add_node("entity.set_position");
    CHECK_FALSE(g.connect(num, "value", pos, "position")); // number -> vector
    CHECK_FALSE(g.connect(start, "out", pos, "position"));  // exec -> data
    CHECK_FALSE(g.connect(a, "nope", b, "in"));
    g.remove_node(a);
    CHECK(g.links.size() == 1);
    g.disconnect_input(b, "in");
    CHECK(g.links.empty());
}

TEST_CASE("graph: validation reports errors on the right nodes") {
    {
        ScriptGraph g;
        const auto n = g.add_node("does.not_exist");
        CHECK(has_error(validate_graph(g), n, "unknown node type"));
    }
    {
        ScriptGraph g;
        g.add_node("event.update");
        const auto dup = g.add_node("event.update");
        CHECK(has_error(validate_graph(g), dup, "more than once"));
    }
    {
        ScriptGraph g; // exec loop
        const auto start = g.add_node("event.start");
        const auto a = g.add_node("debug.log");
        const auto b = g.add_node("debug.log");
        g.connect(start, "out", a, "in");
        g.connect(a, "out", b, "in");
        g.connect(b, "out", a, "in");
        CHECK(has_error(validate_graph(g), a, "exec loop"));
    }
    {
        ScriptGraph g; // data loop through two pure nodes
        const auto start = g.add_node("event.start");
        const auto log = g.add_node("debug.log");
        const auto x = g.add_node("math.add");
        const auto y = g.add_node("math.add");
        g.connect(start, "out", log, "in");
        g.connect(x, "result", log, "message");
        g.connect(y, "result", x, "a");
        g.connect(x, "result", y, "a");
        CHECK(has_error(validate_graph(g), 0, "data loop"));
    }
    {
        ScriptGraph g; // a statement output used from another event
        const auto start = g.add_node("event.start");
        const auto spawn = g.add_node("scene.spawn");
        const auto update = g.add_node("event.update");
        const auto destroy = g.add_node("entity.destroy");
        g.connect(start, "out", spawn, "in");
        g.connect(update, "out", destroy, "in");
        g.connect(spawn, "entity", destroy, "entity");
        CHECK(has_error(validate_graph(g), destroy, "same event"));
    }
    {
        ScriptGraph g; // links written by hand: type mismatch and two links into one input
        const auto start = g.add_node("event.start");
        const auto pos = g.add_node("entity.set_position");
        const auto n1 = g.add_node("const.number");
        const auto v1 = g.add_node("const.vector");
        const auto v2 = g.add_node("const.vector");
        g.connect(start, "out", pos, "in");
        g.links.push_back({n1, "value", pos, "position"});
        CHECK(has_error(validate_graph(g), pos, "cannot connect number"));
        g.links.pop_back();
        g.links.push_back({v1, "value", pos, "position"});
        g.links.push_back({v2, "value", pos, "position"});
        CHECK(has_error(validate_graph(g), pos, "only have one link"));
        g.links.push_back({start, "out", n1, "in"});
        CHECK(has_error(validate_graph(g), n1, "does not have"));
    }
    {
        ScriptGraph g; // variables
        g.variables.push_back({"update", PinType::Number, {}, true});
        g.variables.push_back({"_hidden", PinType::Number, {}, true});
        g.variables.push_back({"ok", PinType::Number, {}, true});
        g.variables.push_back({"ok", PinType::Bool, {}, true});
        const auto get = g.add_node("variable.get");
        g.node(get)->config.set("variable", "missing");
        const auto diags = validate_graph(g);
        CHECK(has_error(diags, 0, "variable name 'update'"));
        CHECK(has_error(diags, 0, "variable name '_hidden'"));
        CHECK(has_error(diags, 0, "declared twice"));
        CHECK(has_error(diags, get, "unknown variable"));
    }
    {
        ScriptGraph g; // unreachable statement: a warning, still compiles
        g.add_node("event.start");
        const auto orphan = g.add_node("debug.log");
        const CompiledGraph c = compile_graph(g);
        CHECK(c.ok());
        REQUIRE(c.diagnostics.size() == 1);
        CHECK(c.diagnostics[0].severity == GraphDiagnostic::Severity::Warning);
        CHECK(c.diagnostics[0].node == orphan);
    }
}

TEST_CASE("graph: compiled source maps lines to nodes and literals are escaped") {
    ScriptGraph g;
    const auto start = g.add_node("event.start");
    const auto log = g.add_node("debug.log");
    g.node(log)->inputs.set("message", "quote \" backslash \\ newline \n end");
    g.connect(start, "out", log, "in");
    const CompiledGraph c = compile_graph(g, {.source_name = "scripts/t.vgraph"});
    REQUIRE(c.ok());
    CHECK(check_syntax(c.source, "t").ok());
    CHECK(c.source.find("\\\"") != std::string::npos);
    CHECK(c.source.find("\\n") != std::string::npos);
    std::uint32_t log_line = 0;
    for (std::uint32_t i = 1; i <= c.line_nodes.size(); ++i)
        if (c.node_at_line(i) == log) log_line = i;
    CHECK(log_line > 0);
    CHECK(c.node_at_line(0) == 0);
    CHECK(c.node_at_line(99999) == 0);
    const std::string msg = annotate_graph_error(std::format("scripts/t.vgraph:{}: boom", log_line), c, g);
    CHECK(msg.find(std::format("[node {} Log]", log)) != std::string::npos);
    const CompiledGraph traced = compile_graph(g, {.source_name = "t", .trace = true});
    CHECK(traced.source.find(std::format("__vg_trace({})", log)) != std::string::npos);
    CHECK(check_syntax(traced.source, "t").ok());
}

TEST_CASE("graph: compiled graphs run in the script host with per-entity properties") {
    GraphDir d;
    d.write("scripts/mover.vgraph", serialize_graph(mover_graph()));
    // Loop/once/sequence graph: start sums 1..4 into total, once adds 100, sequence logs.
    ScriptGraph flow;
    flow.variables.push_back({"total", PinType::Number, json::Value(0), true});
    const auto start = flow.add_node("event.start");
    const auto seq = flow.add_node("flow.sequence");
    const auto loop = flow.add_node("flow.for_range");
    flow.node(loop)->inputs.set("last", 4);
    const auto get = flow.add_node("variable.get");
    flow.node(get)->config.set("variable", "total");
    const auto add = flow.add_node("math.add");
    const auto set = flow.add_node("variable.set");
    flow.node(set)->config.set("variable", "total");
    const auto once = flow.add_node("flow.do_once");
    const auto get2 = flow.add_node("variable.get");
    flow.node(get2)->config.set("variable", "total");
    const auto add2 = flow.add_node("math.add");
    flow.node(add2)->inputs.set("b", 100);
    const auto set2 = flow.add_node("variable.set");
    flow.node(set2)->config.set("variable", "total");
    REQUIRE(flow.connect(start, "out", seq, "in"));
    REQUIRE(flow.connect(seq, "then_0", loop, "in"));
    REQUIRE(flow.connect(loop, "body", set, "in"));
    REQUIRE(flow.connect(get, "value", add, "a"));
    REQUIRE(flow.connect(loop, "index", add, "b"));
    REQUIRE(flow.connect(add, "result", set, "value"));
    REQUIRE(flow.connect(seq, "then_1", once, "in"));
    REQUIRE(flow.connect(once, "out", set2, "in"));
    REQUIRE(flow.connect(get2, "value", add2, "a"));
    REQUIRE(flow.connect(add2, "result", set2, "value"));
    d.write("scripts/flow.vgraph", serialize_graph(flow));

    script::register_components();
    Scene s;
    const Entity a = s.create("A");
    s.world().add<ScriptComponent>(a).script = "scripts/mover.vgraph";
    const Entity b = s.create("B");
    auto& bc = s.world().add<ScriptComponent>(b);
    bc.script = "scripts/mover.vgraph";
    bc.properties.set("speed", 10);
    const Entity c = s.create("C");
    s.world().add<ScriptComponent>(c).script = "scripts/flow.vgraph";
    HostOptions o;
    o.assets_root = d.path;
    auto host = ScriptHost::create(o);
    REQUIRE(host.ok());
    Keys keys;
    gameplay::FrameTime t;
    t.dt = 0.5f;
    (*host)->update(s, t, &keys); // D not held: nothing moves
    CHECK(s.world().get<scene::Transform>(a)->position.x == doctest::Approx(0.0f));
    keys.down.insert("D");
    for (int i = 0; i < 4; ++i) (*host)->update(s, t, &keys);
    CHECK(s.world().get<scene::Transform>(a)->position.x == doctest::Approx(4.0f));  // 2 * 0.5 * 4
    CHECK(s.world().get<scene::Transform>(b)->position.x == doctest::Approx(20.0f)); // per-entity speed
    const std::string last_error = (*host)->recent_errors().empty() ? std::string() : (*host)->recent_errors().back();
    INFO(last_error);
    CHECK((*host)->stats().errors == 0);
    auto count = (*host)->eval("return 0"); // host still healthy
    CHECK(count.ok());
    // Exposed variables are editable properties; private ones are hidden from the inspector.
    auto props = (*host)->describe("scripts/mover.vgraph");
    REQUIRE(props.ok());
    REQUIRE(props->size() == 1);
    CHECK((*props)[0].name == "speed");
    CHECK((*props)[0].default_value.as_number() == 2);
    auto flow_props = (*host)->describe("scripts/flow.vgraph");
    REQUIRE(flow_props.ok());
    REQUIRE(flow_props->size() == 1);
    // luau_source: graphs compile, plain scripts pass through, broken graphs report their node.
    auto src = luau_source("scripts/mover.vgraph", serialize_graph(mover_graph()));
    REQUIRE(src.ok());
    CHECK(src->find("function M:update(") != std::string::npos);
    CHECK(luau_source("a.luau", "return {}").value() == "return {}");
    ScriptGraph broken;
    const auto bad = broken.add_node("nope");
    auto err = luau_source("scripts/broken.vgraph", serialize_graph(broken));
    REQUIRE_FALSE(err.ok());
    CHECK(err.error().message.find(std::format("node {}", bad)) != std::string::npos);
}

TEST_CASE("graph: runtime errors name the failing node") {
    GraphDir d;
    ScriptGraph g;
    const auto start = g.add_node("event.start");
    const auto add = g.add_node("math.add");
    g.node(add)->inputs.set("a", "text");
    g.node(add)->inputs.set("b", true);
    const auto log = g.add_node("debug.log");
    g.connect(start, "out", log, "in");
    g.connect(add, "result", log, "message");
    d.write("scripts/bad.vgraph", serialize_graph(g));
    script::register_components();
    Scene s;
    const Entity e = s.create("E");
    s.world().add<ScriptComponent>(e).script = "scripts/bad.vgraph";
    HostOptions o;
    o.assets_root = d.path;
    auto host = ScriptHost::create(o);
    REQUIRE(host.ok());
    (*host)->update(s, {}, nullptr);
    REQUIRE_FALSE((*host)->recent_errors().empty());
    const std::string& msg = (*host)->recent_errors().back();
    INFO(msg);
    CHECK(msg.find(std::format("[node {} Log]", log)) != std::string::npos);
    CHECK((*host)->stats().faulted == 1);
}

TEST_CASE("graph: traced hosts count node runs and keep the last error per graph") {
    GraphDir d;
    d.write("scripts/mover.vgraph", serialize_graph(mover_graph()));
    ScriptGraph bad;
    const auto start = bad.add_node("event.start");
    const auto add = bad.add_node("math.add");
    bad.node(add)->inputs.set("a", "text");
    bad.node(add)->inputs.set("b", true);
    const auto log = bad.add_node("debug.log");
    bad.connect(start, "out", log, "in");
    bad.connect(add, "result", log, "message");
    d.write("scripts/bad.vgraph", serialize_graph(bad));
    script::register_components();
    Scene s;
    const Entity a = s.create("A");
    s.world().add<ScriptComponent>(a).script = "scripts/mover.vgraph";
    const Entity b = s.create("B");
    s.world().add<ScriptComponent>(b).script = "scripts/bad.vgraph";
    Keys keys;
    HostOptions o;
    o.assets_root = d.path;
    o.trace_graphs = true;
    auto host = ScriptHost::create(o);
    REQUIRE(host.ok());
    gameplay::FrameTime t;
    t.dt = 0.5f;
    // Three frames without D, two with: the event runs 5 times, the branch body twice.
    for (int i = 0; i < 5; ++i) {
        if (i == 3) keys.down.insert("D");
        (*host)->update(s, t, &keys);
    }
    const ScriptGraph g = mover_graph();
    const auto id_of = [&](std::string_view type) {
        for (const auto& n : g.nodes)
            if (n.type == type) return n.id;
        return 0u;
    };
    const auto hits = (*host)->graph_node_hits("scripts/mover.vgraph");
    CHECK(hits.at(id_of("event.update")) == 5);
    CHECK(hits.at(id_of("flow.branch")) == 5);
    CHECK(hits.at(id_of("entity.translate")) == 2);
    CHECK(hits.at(id_of("variable.set")) == 2);
    CHECK_FALSE(hits.contains(id_of("math.multiply"))); // pure nodes are inlined, not traced
    CHECK((*host)->last_graph_error("scripts/mover.vgraph") == std::nullopt);

    const auto err = (*host)->last_graph_error("scripts/bad.vgraph");
    REQUIRE(err);
    CHECK(err->node == log);
    CHECK(err->message.find("[node") != std::string::npos);
    CHECK(err->serial >= 1);
    CHECK((*host)->graph_node_hits("scripts/bad.vgraph").at(start) == 1);

    // Fixing the graph and reloading clears its error.
    bad.node(add)->inputs.set("a", 1);
    bad.node(add)->inputs.set("b", 2);
    d.write("scripts/bad.vgraph", serialize_graph(bad));
    (void)(*host)->reload_changed();
    CHECK((*host)->last_graph_error("scripts/bad.vgraph") == std::nullopt);

    // Untraced hosts record nothing.
    HostOptions plain;
    plain.assets_root = d.path;
    auto quiet = ScriptHost::create(plain);
    REQUIRE(quiet.ok());
    (*quiet)->update(s, t, &keys);
    CHECK((*quiet)->graph_node_hits("scripts/mover.vgraph").empty());
}
