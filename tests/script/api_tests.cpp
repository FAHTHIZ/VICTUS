// SPDX-License-Identifier: Apache-2.0
// Versioned script API (ADR-0155): version rules, the live description, the frozen snapshots and
// hosts refusing scripts written for an incompatible API. Also the CPU-time call budget.
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/script/api.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

using namespace victus;
using script::ApiVersion;

namespace {

json::Value snapshot(std::string_view version, std::initializer_list<const char*> scene_fns,
                     std::initializer_list<std::pair<const char*, const char*>> props = {{"position", "rw"}}) {
    json::Value doc = json::Value::object();
    doc.set("format", "victus.script_api");
    doc.set("version", std::string(version));
    json::Value scene = json::Value::object();
    for (const char* f : scene_fns) scene.set(f, "function");
    json::Value globals = json::Value::object();
    globals.set("scene", std::move(scene));
    doc.set("globals", std::move(globals));
    json::Value entity = json::Value::object();
    json::Value methods = json::Value::array();
    methods.push("get");
    entity.set("methods", std::move(methods));
    json::Value p = json::Value::object();
    for (const auto& [name, access] : props) p.set(name, access);
    entity.set("properties", std::move(p));
    json::Value types = json::Value::object();
    types.set("Entity", std::move(entity));
    doc.set("types", std::move(types));
    json::Value cb = json::Value::array();
    cb.push("update");
    doc.set("callbacks", std::move(cb));
    return doc;
}

} // namespace

TEST_CASE("script api: versions parse and compare") {
    CHECK(script::parse_api_version("1").value() == ApiVersion{1, 0});
    CHECK(script::parse_api_version("1.0").value() == ApiVersion{1, 0});
    CHECK(script::parse_api_version("2.13").value() == ApiVersion{2, 13});
    for (const char* bad : {"", ".", "1.", ".1", "0", "0.1", "01", "1.02", "1.2.3", "a", "1.x", "-1", "1 .0"})
        CHECK_MESSAGE(!script::parse_api_version(bad).ok(), bad);
    CHECK(script::to_string({3, 4}) == "3.4");
    CHECK(script::snapshot_file_name({1, 2}) == "victus_script_api_1.2.json");

    CHECK(script::api_compatible({1, 0}, {1, 0}));
    CHECK(script::api_compatible({1, 0}, {1, 3}));
    CHECK_FALSE(script::api_compatible({1, 4}, {1, 3}));
    CHECK_FALSE(script::api_compatible({1, 0}, {2, 0}));
    CHECK_FALSE(script::api_compatible({2, 0}, {1, 9}));
    CHECK(script::check_api_requirement(script::kScriptApiVersion).ok());
    const auto newer = script::check_api_requirement({1, 5}, {1, 3});
    REQUIRE_FALSE(newer.ok());
    CHECK(newer.error().code == ErrorCode::VersionMismatch);
    CHECK(newer.error().message.find("newer") != std::string::npos);
    const auto major = script::check_api_requirement({2, 0}, {1, 3});
    REQUIRE_FALSE(major.ok());
    CHECK(major.error().message.find("ported") != std::string::npos);
}

TEST_CASE("script api: compatibility rules between snapshots") {
    const auto v10 = snapshot("1.0", {"find", "spawn"});
    CHECK(script::check_compatibility(v10, v10).ok());
    // Additions need a minor bump.
    CHECK_FALSE(script::check_compatibility(v10, snapshot("1.0", {"find", "spawn", "count"})).ok());
    CHECK(script::check_compatibility(v10, snapshot("1.1", {"find", "spawn", "count"})).ok());
    // Removals and narrowing a writable member need a major bump.
    const auto removed = script::check_compatibility(v10, snapshot("1.1", {"find"}));
    REQUIRE_FALSE(removed.ok());
    CHECK(removed.error().message.find("scene.spawn") != std::string::npos);
    const auto narrowed = script::check_compatibility(v10, snapshot("1.1", {"find", "spawn"}, {{"position", "r"}}));
    REQUIRE_FALSE(narrowed.ok());
    CHECK(narrowed.error().message.find("Entity.position=") != std::string::npos);
    CHECK(script::check_compatibility(v10, snapshot("2.0", {"find"})).ok());
    // Versions do not go down, and do not change without an API change.
    CHECK_FALSE(script::check_compatibility(snapshot("1.1", {"find"}), snapshot("1.0", {"find"})).ok());
    CHECK_FALSE(script::check_compatibility(v10, snapshot("1.1", {"find", "spawn"})).ok());
    // Widening read-only to read-write is an addition.
    const auto ro = snapshot("1.0", {"find"}, {{"position", "r"}});
    auto d = script::diff_api(ro, snapshot("1.1", {"find"}, {{"position", "rw"}}));
    REQUIRE(d.ok());
    CHECK(d->added == std::set<std::string>{"Entity.position="});
    CHECK(d->removed.empty());

    json::Value junk = json::Value::object();
    junk.set("format", "something");
    CHECK_FALSE(script::api_symbols(junk).ok());
}

TEST_CASE("script api: the live description covers the VICTUS bindings only") {
    auto api = script::describe_api();
    REQUIRE(api.ok());
    CHECK(script::snapshot_version(*api).value() == script::kScriptApiVersion);
    auto symbols = script::api_symbols(*api);
    REQUIRE(symbols.ok());
    for (const char* s : {"scene.find", "scene.instantiate", "input.action", "physics.raycast", "save.checkpoint",
                          "require", "Entity:get", "Entity:play_animation", "Entity.valid", "Entity.position",
                          "Entity.position=", "Entity.id", "Entity.grounded", "Bridge:request", "callback:update",
                          "callback:on_trigger_enter", "callback:load_state"})
        CHECK_MESSAGE(symbols->contains(s), s);
    // Read-only members are not writable; Luau's own libraries and tooling hooks are excluded.
    for (const char* s : {"Entity.id=", "Entity.valid=", "Entity.forward=", "math", "math.floor", "string.format",
                          "print", "vector.create", "__vg_trace_for"})
        CHECK_MESSAGE(!symbols->contains(s), s);
}

TEST_CASE("script api: the bindings match the frozen snapshots") {
    auto api = script::describe_api();
    REQUIRE(api.ok());
    const auto st = script::check_snapshot_folder(VICTUS_SCRIPT_API_DIR, *api);
    CHECK_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

TEST_CASE("script api: snapshot folder errors name the fix") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_script_api_folder_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto api = script::describe_api();
    REQUIRE(api.ok());
    auto missing = script::check_snapshot_folder(dir, *api);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("victus script-api --write") != std::string::npos);

    // A frozen snapshot that lacks a binding: the error lists it and the version it needs.
    json::Value frozen = *api;
    json::Value globals = frozen["globals"];
    auto& obj = globals.as_object();
    obj.erase(std::remove_if(obj.begin(), obj.end(), [](const auto& kv) { return kv.first == "water"; }), obj.end());
    frozen.set("globals", std::move(globals));
    REQUIRE(fs::write_text_atomic(dir / script::snapshot_file_name(script::kScriptApiVersion), json::stringify(frozen)).ok());
    auto added = script::check_snapshot_folder(dir, *api);
    REQUIRE_FALSE(added.ok());
    CHECK(added.error().message.find("+water.query") != std::string::npos);
    CHECK(added.error().message.find(script::to_string({script::kScriptApiVersion.major, script::kScriptApiVersion.minor + 1})) !=
          std::string::npos);

    // A misnamed snapshot is rejected.
    REQUIRE(fs::write_text_atomic(dir / "victus_script_api_9.9.json", json::stringify(*api)).ok());
    CHECK_FALSE(script::check_snapshot_folder(dir, *api).ok());
    std::filesystem::remove_all(dir);
}

TEST_CASE("script api: hosts refuse scripts written for an incompatible API") {
    script::HostOptions ok;
    ok.script_api = {script::kScriptApiVersion.major, 0};
    CHECK(script::ScriptHost::create(ok).ok());
    for (ApiVersion bad : {ApiVersion{script::kScriptApiVersion.major + 1, 0},
                           ApiVersion{script::kScriptApiVersion.major, script::kScriptApiVersion.minor + 1}}) {
        script::HostOptions o;
        o.script_api = bad;
        auto host = script::ScriptHost::create(o);
        REQUIRE_FALSE(host.ok());
        CHECK(host.error().code == ErrorCode::VersionMismatch);
    }
}

TEST_CASE("script budget: time the thread is blocked or descheduled is not charged") {
    // plugins.call blocks the script thread without using CPU, like a preempted thread on a
    // loaded machine. The budget is 50 ms; the call blocks 300 ms and the script then keeps
    // running (enough loop iterations to pass many budget checks). Wall-clock budgets killed it.
    script::HostOptions o;
    o.call_budget_seconds = 0.05;
    o.plugin_has = [](std::string_view) { return true; };
    o.plugin_call = [](std::string_view, std::string_view) -> Result<std::string> {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return std::string("1");
    };
    auto host = script::ScriptHost::create(o);
    REQUIRE(host.ok());
    auto r = (*host)->eval("local x = plugins.call('block', '0') local s = 0 for i = 1, 200000 do s += i end return s");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r->as_number() == 200000.0 * 200001.0 / 2.0);

    // A script that really uses its CPU budget is still stopped, and promptly.
    const auto t0 = std::chrono::steady_clock::now();
    auto spin = (*host)->eval("while true do end");
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE_FALSE(spin.ok());
    CHECK(spin.error().message.find("time budget") != std::string::npos);
    CHECK(took < 2.0);
    // Blocking before a busy loop does not buy the loop extra time.
    auto both = (*host)->eval("plugins.call('block', '0') while true do end");
    REQUIRE_FALSE(both.ok());
    CHECK(both.error().message.find("time budget") != std::string::npos);
}
