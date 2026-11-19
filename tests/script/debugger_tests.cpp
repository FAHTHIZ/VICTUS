// SPDX-License-Identifier: Apache-2.0
// Luau source debugger (ADR-0099): breakpoints, line resolution, stepping, pause, conditions,
// hit counts, logpoints, stops on errors, inspection, evaluation and assignment.
#include "victus/core/fs.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/script/dap.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <deque>
#include <filesystem>
#include <thread>
#include <vector>

using namespace victus;
using scene::Entity;
using scene::Scene;
using script::DebugStop;
using script::ScriptHost;
using script::SourceBreakpoint;
using script::StepAction;
using script::StopReason;

namespace {

// Line numbers matter: the tests set breakpoints on them.
constexpr const char* kScript = R"lua(local Dbg = {}
local counter = 0
local function add(a, b)
    local sum = a + b
    return sum
end

function Dbg:update(dt)
    counter += 1
    local t = { x = 1, y = "two", list = {10, 20, 30} }
    local r = add(counter, 10)
    self.entity.name = "r" .. r
    if counter == self.fail_at then error("boom " .. counter) end
end

return Dbg
)lua";

constexpr const char* kPath = "scripts/dbg.luau";

struct Fixture {
    std::filesystem::path dir;
    Scene scene;
    Entity e;
    std::unique_ptr<ScriptHost> host;
    gameplay::Simulation sim{1.0f / 60.0f};

    explicit Fixture(bool debugger = true, double budget = 0.25, int fail_at = 0) {
        dir = std::filesystem::temp_directory_path() / ("victus_dbg_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(dir / "scripts");
        REQUIRE(fs::write_text_atomic(dir / kPath, kScript).ok());
        script::register_components();
        e = scene.create("E");
        auto& c = scene.world().add<script::ScriptComponent>(e);
        c.script = kPath;
        json::Value props = json::Value::object();
        props.set("fail_at", fail_at);
        c.properties = props;
        script::HostOptions o;
        o.assets_root = dir;
        o.call_budget_seconds = budget;
        o.debugger = debugger;
        auto h = ScriptHost::create(o);
        REQUIRE(h.ok());
        host = std::move(h).value();
        script::add_script_systems(sim, *host);
    }
    ~Fixture() {
        host.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void tick() { sim.tick(scene, 1.0f / 60.0f); }
    std::string name() const { return scene.world().get<scene::Name>(e)->value; }
};

struct Stop {
    StopReason reason;
    std::string function;
    std::string source;
    int line;
    std::size_t depth;
    std::string message;
};

// Records every stop and answers with the next planned action (Continue when the plan runs out).
struct Recorder {
    std::vector<Stop> stops;
    std::vector<StepAction> plan;
    std::function<void(DebugStop&)> inspect;
    StepAction operator()(DebugStop& s) {
        const auto& f = s.frames();
        REQUIRE(!f.empty());
        stops.push_back({s.reason(), f[0].function, f[0].source, f[0].line, f.size(), s.message()});
        if (inspect) inspect(s);
        if (stops.size() <= plan.size()) return plan[stops.size() - 1];
        return StepAction::Continue;
    }
};

const script::DebugVariable* find(const std::vector<script::DebugVariable>& vars, std::string_view name) {
    for (const auto& v : vars)
        if (v.name == name) return &v;
    return nullptr;
}

} // namespace

TEST_CASE("script debugger: breakpoints resolve, stop with stack, locals, upvalues and tables") {
    Fixture fx;
    auto rec = std::make_shared<Recorder>();
    rec->inspect = [](DebugStop& s) {
        const auto& f = s.frames();
        if (f[0].function != "update") return; // the module body (line 8 defines update)
        CHECK(f[0].source == kPath);
        const auto locals = s.locals(0);
        REQUIRE(find(locals, "self"));
        REQUIRE(find(locals, "dt"));
        const auto* t = find(locals, "t");
        REQUIRE(t);
        CHECK(t->type == "table");
        CHECK(t->reference > 0);
        CHECK(t->value.find("x = 1") != std::string::npos);
        const auto kids = s.children(t->reference);
        REQUIRE(kids.size() == 3); // sorted names
        CHECK(kids[0].name == "list");
        CHECK(kids[1].name == "x");
        CHECK(kids[1].value == "1");
        CHECK(kids[2].value == "\"two\"");
        const auto list = s.children(kids[0].reference);
        REQUIRE(list.size() == 3);
        CHECK(list[0].name == "[1]");
        CHECK(list[2].value == "30");
        const auto ups = s.upvalues(0);
        REQUIRE(find(ups, "counter"));
        CHECK(find(ups, "add"));
        auto v = s.evaluate("counter * 2 + t.list[2]");
        REQUIRE(v.ok());
        CHECK(v->type == "number");
        // The instance shows its module as metatable.
        const auto self_kids = s.children(find(locals, "self")->reference);
        CHECK(find(self_kids, "[metatable]"));
        CHECK(find(self_kids, "entity"));
        CHECK(!s.evaluate("this is not luau").ok());
        CHECK(!s.evaluate("error('nope')").ok());
    };
    fx.host->set_debug_handler([rec](DebugStop& s) { return (*rec)(s); });
    // Not loaded yet: unverified. Line 7 is blank and moves to the next line with code.
    auto st = fx.host->set_breakpoints(kPath, {{.line = 11}, {.line = 7}, {.line = 400}});
    REQUIRE(st.size() == 3);
    CHECK(!st[0].verified);
    fx.tick();
    st = fx.host->breakpoints(kPath);
    CHECK(st[0].verified);
    CHECK(st[0].line == 11);
    CHECK(st[1].verified);
    CHECK(st[1].line > 7);
    CHECK(!st[2].verified);
    CHECK(!st[2].message.empty());
    // Line 7 resolved to the function definition, which runs when the module loads; line 11
    // stops once per update.
    const std::size_t after_first = rec->stops.size();
    CHECK(after_first >= 1);
    CHECK(rec->stops.back().line == 11);
    fx.tick();
    fx.tick();
    CHECK(rec->stops.size() == after_first + 2);
    CHECK(fx.name() == "r13");
    CHECK(fx.host->debug_stats().stops == rec->stops.size());
    // Clearing them stops nothing.
    CHECK(fx.host->set_breakpoints(kPath, {}).empty());
    fx.tick();
    CHECK(rec->stops.size() == after_first + 2);
    CHECK(fx.name() == "r14");
    CHECK(fx.host->stats().errors == 0);
}

TEST_CASE("script debugger: step over, into and out from a breakpoint") {
    Fixture fx;
    auto rec = std::make_shared<Recorder>();
    rec->plan = {StepAction::StepIn, StepAction::StepOver, StepAction::StepOut, StepAction::StepOver,
                 StepAction::StepOver};
    fx.host->set_debug_handler([rec](DebugStop& s) { return (*rec)(s); });
    fx.host->set_breakpoints(kPath, {{.line = 11}});
    fx.tick();
    REQUIRE(rec->stops.size() >= 5);
    const auto& s = rec->stops;
    CHECK(s[0].reason == StopReason::Breakpoint);
    CHECK(s[0].line == 11);
    // Step in: inside add.
    CHECK(s[1].reason == StopReason::Step);
    CHECK(s[1].function == "add");
    CHECK(s[1].line == 4);
    CHECK(s[1].depth == s[0].depth + 1);
    // Step over within add.
    CHECK(s[2].line == 5);
    CHECK(s[2].function == "add");
    // Step out: back in update at its next line (returning into the middle of line 11 is not a
    // new line, as with Lua line hooks).
    CHECK(s[3].function == "update");
    CHECK(s[3].line == 12);
    CHECK(s[3].depth == s[0].depth);
    // Step over: the next line of update.
    CHECK(s[4].line == 13);
    CHECK(s[4].function == "update");
    CHECK(fx.name() == "r11");
    CHECK(fx.host->stats().errors == 0);
}

TEST_CASE("script debugger: pause, stepping across calls and single-step runs") {
    Fixture fx;
    auto rec = std::make_shared<Recorder>();
    rec->plan = {StepAction::StepOver, StepAction::StepOver, StepAction::StepOver};
    fx.host->set_debug_handler([rec](DebugStop& s) { return (*rec)(s); });
    fx.tick(); // loads and runs once without stopping
    REQUIRE(rec->stops.empty());
    fx.host->request_pause(); // from any thread
    std::thread([&] { fx.host->request_pause(); }).join();
    fx.tick();
    REQUIRE(rec->stops.size() >= 4);
    CHECK(rec->stops[0].reason == StopReason::Pause);
    CHECK(rec->stops[0].function == "update");
    CHECK(rec->stops[0].line == 9);
    CHECK(rec->stops[1].line == 10);
    CHECK(rec->stops[2].line == 11);
    CHECK(rec->stops[3].line == 12);
    CHECK(rec->stops[3].reason == StopReason::Step);
    // A step off the end of a call stops at the first line of the next call.
    Fixture fx2;
    auto rec2 = std::make_shared<Recorder>();
    rec2->plan = std::vector<StepAction>(6, StepAction::StepOver);
    fx2.host->set_debug_handler([rec2](DebugStop& s) { return (*rec2)(s); });
    fx2.host->set_breakpoints(kPath, {{.line = 13}});
    fx2.tick();
    const std::size_t n1 = rec2->stops.size();
    REQUIRE(n1 >= 1);
    CHECK(rec2->stops[0].line == 13);
    fx2.tick();
    REQUIRE(rec2->stops.size() > n1);
    CHECK(rec2->stops[n1].reason == StopReason::Step);
    CHECK(rec2->stops[n1].function == "update");
    CHECK(rec2->stops[n1].line == 9);
    CHECK(fx2.host->stats().errors == 0);
}

TEST_CASE("script debugger: conditions, hit counts and logpoints") {
    Fixture fx;
    auto rec = std::make_shared<Recorder>();
    std::vector<std::string> logs;
    fx.host->set_debug_handler([rec](DebugStop& s) { return (*rec)(s); },
                               [&logs](std::string_view t) { logs.emplace_back(t); });
    fx.host->set_breakpoints(kPath, {{.line = 11, .condition = "counter == 2"},
                                     {.line = 12, .log_message = "count={counter} name={t.y} bad={nope.x}"},
                                     {.line = 12, .hit_condition = ">=3"}});
    for (int i = 0; i < 4; ++i) fx.tick();
    REQUIRE(rec->stops.size() == 3);
    CHECK(rec->stops[0].line == 11);
    CHECK(rec->stops[1].line == 12); // third hit of line 12
    CHECK(rec->stops[2].line == 12); // fourth
    REQUIRE(logs.size() == 4);
    CHECK(logs[0].starts_with("count=1 name=two bad={"));
    CHECK(logs[3].starts_with("count=4 "));
    CHECK(fx.host->debug_stats().log_messages == 4);
    // A failing condition stops and says why.
    fx.host->set_breakpoints(kPath, {{.line = 11, .condition = "nope.x > 1"}});
    fx.tick();
    REQUIRE(rec->stops.size() == 4);
    CHECK(rec->stops[3].message.find("condition") != std::string::npos);
}

TEST_CASE("script debugger: uncaught errors stop with the failing frame, variables can be set") {
    Fixture fx(true, 0.25, 3);
    auto rec = std::make_shared<Recorder>();
    rec->inspect = [](DebugStop& s) {
        if (s.reason() == StopReason::Exception) {
            CHECK(s.message().find("boom 3") != std::string::npos);
            CHECK(s.frames()[0].line == 13);
            const auto ups = s.upvalues(0);
            REQUIRE(find(ups, "counter"));
            CHECK(find(ups, "counter")->value == "3");
            return;
        }
        // At line 12 on the second update: r is 12; make it 99.
        auto v = s.set_variable(0, "r", "90 + 9");
        REQUIRE(v.ok());
        CHECK(v->value == "99");
        CHECK(!s.set_variable(0, "missing", "1").ok());
        auto c = s.set_variable(0, "counter", "counter"); // upvalue, unchanged
        CHECK(c.ok());
    };
    fx.host->set_debug_handler([rec](DebugStop& s) { return (*rec)(s); });
    fx.host->set_break_on_error(true);
    fx.tick();
    fx.host->set_breakpoints(kPath, {{.line = 12}});
    fx.tick();
    CHECK(fx.name() == "r99");
    fx.host->set_breakpoints(kPath, {});
    fx.tick();
    REQUIRE(rec->stops.size() == 2);
    CHECK(rec->stops[1].reason == StopReason::Exception);
    // The error is still reported as before.
    REQUIRE(!fx.host->recent_errors().empty());
    CHECK(fx.host->recent_errors().back().find("boom 3") != std::string::npos);
}

TEST_CASE("script debugger: time stopped does not count against the call budget; off by default") {
    Fixture fx(true, 0.05);
    fx.host->set_debug_handler([](DebugStop&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return StepAction::Continue;
    });
    fx.host->set_breakpoints(kPath, {{.line = 11}});
    fx.tick();
    fx.tick();
    CHECK(fx.host->stats().errors == 0);
    CHECK(fx.host->debug_stats().stops == 2);

    Fixture off(false);
    int stops = 0;
    off.host->set_debug_handler([&stops](DebugStop&) {
        ++stops;
        return StepAction::Continue;
    });
    off.host->request_pause();
    auto st = off.host->set_breakpoints(kPath, {{.line = 11}});
    off.tick();
    off.tick();
    CHECK(stops == 0);
    REQUIRE(st.size() == 1);
    CHECK(!st[0].verified);
    CHECK(st[0].message.find("HostOptions::debugger") != std::string::npos);
    CHECK(off.name() == "r12");
}

TEST_CASE("script debugger: DAP framing decodes split and back-to-back messages") {
    json::Value a = json::Value::object();
    a.set("seq", 1);
    a.set("command", "initialize");
    json::Value b = json::Value::object();
    b.set("seq", 2);
    b.set("text", "h\u00e9llo \r\n\r\n");
    const std::string bytes = script::dap_encode(a) + script::dap_encode(b);
    CHECK(bytes.starts_with("Content-Length: "));
    script::DapDecoder d;
    std::vector<json::Value> got;
    for (char ch : bytes) { // one byte at a time
        d.feed(std::string_view(&ch, 1));
        auto m = d.next();
        REQUIRE(m);
        if (*m) got.push_back(**m);
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0] == a);
    CHECK(got[1] == b);
    script::DapDecoder bad;
    bad.feed("Content-Type: x\r\n\r\n{}");
    CHECK_FALSE(bad.next());
}

TEST_CASE("script debugger: DAP session answers inspection requests while stopped") {
    Fixture fx;
    std::vector<json::Value> sent;
    std::deque<json::Value> inbox;
    std::int64_t seq = 0;
    auto request = [&](std::string command, json::Value args = json::Value::object()) {
        json::Value r = json::Value::object();
        r.set("seq", ++seq);
        r.set("type", "request");
        r.set("command", std::move(command));
        r.set("arguments", std::move(args));
        return r;
    };
    script::DapSession dap([&](const json::Value& m) { sent.push_back(m); },
                           [&]() -> std::optional<json::Value> {
                               if (inbox.empty()) return std::nullopt;
                               json::Value v = inbox.front();
                               inbox.pop_front();
                               return v;
                           });
    auto response = [&](std::string_view command) -> json::Value {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
            if ((*it)["type"].as_string_or("") == "response" && (*it)["command"].as_string_or("") == command) return *it;
        return {};
    };
    auto events = [&](std::string_view name) {
        std::vector<json::Value> out;
        for (const auto& m : sent)
            if (m["type"].as_string_or("") == "event" && m["event"].as_string_or("") == name) out.push_back(m);
        return out;
    };

    CHECK(dap.handle(request("initialize")));
    CHECK(response("initialize")["body"]["supportsConditionalBreakpoints"].as_bool());
    CHECK_FALSE(dap.handle(request("launch"))); // left to the host application
    dap.attach(*fx.host, fx.dir);

    json::Value src = json::Value::object();
    src.set("path", (fx.dir / kPath).string());
    json::Value bp = json::Value::object();
    bp.set("line", 9);
    json::Value bps = json::Value::array();
    bps.push(bp);
    json::Value args = json::Value::object();
    args.set("source", src);
    args.set("breakpoints", bps);
    REQUIRE(dap.handle(request("setBreakpoints", args)));
    CHECK_FALSE(response("setBreakpoints")["body"]["breakpoints"][0]["verified"].as_bool());

    // A path outside the assets folder is rejected.
    json::Value outside = args;
    json::Value osrc = json::Value::object();
    osrc.set("path", (fx.dir.parent_path() / "elsewhere.luau").string());
    outside.set("source", osrc);
    REQUIRE(dap.handle(request("setBreakpoints", outside)));
    CHECK(response("setBreakpoints")["body"]["breakpoints"][0]["message"].as_string_or("").find("assets") != std::string::npos);

    REQUIRE(dap.handle(request("configurationDone")));
    CHECK(dap.configured());

    json::Value frame = json::Value::object();
    frame.set("frameId", 1);
    json::Value locals = json::Value::object();
    locals.set("variablesReference", 1);
    json::Value eval = json::Value::object();
    eval.set("expression", "counter * 100");
    eval.set("frameId", 1);
    json::Value thread = json::Value::object();
    thread.set("threadId", 1);
    inbox.push_back(request("stackTrace", thread));
    inbox.push_back(request("scopes", frame));
    inbox.push_back(request("variables", locals));
    inbox.push_back(request("evaluate", eval));
    inbox.push_back(request("launch")); // not available while stopped
    inbox.push_back(request("continue", thread));
    fx.tick();
    dap.poll();

    const auto changed = events("breakpoint");
    REQUIRE(changed.size() == 1);
    CHECK(changed[0]["body"]["breakpoint"]["verified"].as_bool());
    CHECK(changed[0]["body"]["breakpoint"]["line"].as_int() == 9);
    const auto stopped = events("stopped");
    REQUIRE(stopped.size() == 1);
    CHECK(stopped[0]["body"]["reason"].as_string_or("") == "breakpoint");
    const json::Value st = response("stackTrace")["body"]["stackFrames"];
    REQUIRE(st.size() == 1);
    CHECK(st[0]["name"].as_string_or("") == "update");
    CHECK(st[0]["line"].as_int() == 9);
    CHECK(std::filesystem::path(st[0]["source"]["path"].as_string_or("")).filename() == "dbg.luau");
    CHECK(response("scopes")["body"]["scopes"].size() == 2);
    const json::Value vars = response("variables")["body"]["variables"];
    REQUIRE(vars.size() >= 2);
    CHECK(vars[0]["name"].as_string_or("") == "self");
    CHECK(vars[0]["variablesReference"].as_int() > 0);
    CHECK(vars[1]["name"].as_string_or("") == "dt");
    CHECK(response("evaluate")["body"]["result"].as_string_or("") == "0");
    CHECK_FALSE(response("launch")["success"].as_bool());
    CHECK(dap.stops() == 1);

    // While running, evaluate uses the global environment; disconnect clears breakpoints.
    json::Value g = json::Value::object();
    g.set("expression", "1 + 2");
    g.set("context", "repl");
    REQUIRE(dap.handle(request("evaluate", g)));
    CHECK(response("evaluate")["body"]["result"].as_string_or("") == "3");
    REQUIRE(dap.handle(request("disconnect")));
    CHECK(dap.disconnected());
    dap.detach();
    CHECK(fx.host->breakpoints(kPath).empty());
    fx.tick();
    CHECK(events("stopped").size() == 1);
}
