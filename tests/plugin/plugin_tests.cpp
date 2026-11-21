// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/log.hpp"
#include "victus/plugin/plugin.hpp"
#include "victus/scene/scene.hpp"

#include <filesystem>
#include <format>
#include <fstream>

using namespace victus;
namespace pl = victus::plugin;

namespace {

// Builds a throwaway `plugins/` directory: each plugin gets plugins/<name>/<name>.vplugin plus a
// copy of one of the test libraries (built from tests/plugin/test_plugin.c).
struct PluginDir {
    fs::Path root;
    explicit PluginDir(std::string_view tag) {
        root = std::filesystem::temp_directory_path() / std::format("victus_plugin_tests_{}", tag);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~PluginDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    PluginDir(const PluginDir&) = delete;
    PluginDir& operator=(const PluginDir&) = delete;

    void add(std::string_view name, const char* library, std::string_view extra = "", std::string_view api = "1.0",
             std::string_view version = "1.0.0") const {
        const fs::Path dir = root / std::string(name);
        std::filesystem::create_directories(dir / "bin");
        const fs::Path lib(library);
        std::filesystem::copy_file(lib, dir / "bin" / lib.filename(), std::filesystem::copy_options::overwrite_existing);
        std::ofstream(dir / std::format("{}.vplugin", name))
            << std::format(R"({{"name":"{}","version":"{}","api":"{}","libraries":{{"{}":"bin/{}"}}{}}})", name, version,
                           api, pl::platform_key(), lib.filename().string(), extra);
    }
};

struct LogCapture {
    std::shared_ptr<log::RingSink> ring = std::make_shared<log::RingSink>(4096);
    LogCapture() { log::logger().add_sink(ring); }
    ~LogCapture() { log::logger().remove_sink(ring); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    [[nodiscard]] std::vector<std::string> lines(std::string_view message) const {
        std::vector<std::string> out;
        for (const auto& r : ring->snapshot())
            if (r.message == message) out.push_back(r.category);
        return out;
    }
};

pl::Manifest manifest(std::string name, std::vector<pl::Dependency> deps = {}, std::string_view version = "1.0.0") {
    pl::Manifest m;
    m.name = std::move(name);
    m.version = *pl::parse_version(version);
    m.api = {1, 0, 0};
    m.dependencies = std::move(deps);
    return m;
}

std::vector<std::string> names(const std::vector<pl::Manifest>& ms, const std::vector<std::size_t>& order) {
    std::vector<std::string> out;
    for (std::size_t i : order) out.push_back(ms[i].name);
    return out;
}

} // namespace

TEST_CASE("plugin: versions parse and compare") {
    CHECK(pl::parse_version("1").value() == pl::Version{1, 0, 0});
    CHECK(pl::parse_version("1.2").value() == pl::Version{1, 2, 0});
    CHECK(pl::parse_version("10.20.30").value() == pl::Version{10, 20, 30});
    CHECK_FALSE(pl::parse_version(""));
    CHECK_FALSE(pl::parse_version("1.2.3.4"));
    CHECK_FALSE(pl::parse_version("1.x"));
    CHECK_FALSE(pl::parse_version("1.0.0-beta"));
    CHECK_FALSE(pl::parse_version("1..2"));
    CHECK(pl::Version{1, 2, 0} < pl::Version{1, 10, 0});
    CHECK(pl::api_compatible({1, 0, 0}, {1, 3, 0}));
    CHECK_FALSE(pl::api_compatible({1, 4, 0}, {1, 3, 0}));
    CHECK_FALSE(pl::api_compatible({2, 0, 0}, {1, 9, 0}));
    CHECK_FALSE(pl::api_compatible({0, 9, 0}, {1, 0, 0}));
}

TEST_CASE("plugin: manifests are validated") {
    const fs::Path dir = "/tmp/p";
    auto ok = pl::parse_manifest(R"({"name":"terrain_tools","version":"1.2.3","api":"1.0","description":"d",
        "libraries":{"linux":"bin/libtt.so","windows":"bin\\tt.dll"},"dependencies":{"core_utils":">=1.1"},
        "targets":["editor"],"enabled":false})",
                                 dir);
    REQUIRE(ok);
    CHECK(ok->name == "terrain_tools");
    CHECK(ok->version == pl::Version{1, 2, 3});
    CHECK(ok->entry == "victus_plugin_entry");
    CHECK(ok->libraries.at("linux") == "bin/libtt.so");
    CHECK(ok->libraries.at("windows") == "bin/tt.dll");
    REQUIRE(ok->dependencies.size() == 1);
    CHECK(ok->dependencies[0].min_version == pl::Version{1, 1, 0});
    CHECK(ok->targets == std::vector<std::string>{"editor"});
    CHECK_FALSE(ok->enabled);
    CHECK(ok->dir == dir);

    const char* bad[] = {
        R"([1])",
        R"({"version":"1.0.0","api":"1.0"})",
        R"({"name":"Bad Name","version":"1.0.0","api":"1.0"})",
        R"({"name":"a","version":"one","api":"1.0"})",
        R"({"name":"a","version":"1.0.0"})",
        R"({"name":"a","version":"1.0.0","api":"1.0","entry":"not-an-identifier"})",
        R"({"name":"a","version":"1.0.0","api":"1.0","libraries":{"linux":"../escape.so"}})",
        R"({"name":"a","version":"1.0.0","api":"1.0","libraries":{"linux":"/abs/a.so"}})",
        R"({"name":"a","version":"1.0.0","api":"1.0","dependencies":{"a":"1.0"}})",
        R"({"name":"a","version":"1.0.0","api":"1.0","dependencies":["b"]})",
        R"({"name":"a","version":"1.0.0","api":"1.0","targets":["server"]})",
        R"({"name":"a","version":"1.0.0","api":"1.0","enabled":"yes"})",
        R"({"name":"a", broken)",
    };
    for (const char* text : bad) {
        INFO(text);
        CHECK_FALSE(pl::parse_manifest(text, dir));
    }
}

TEST_CASE("plugin: dependency resolution orders, rejects and detects cycles") {
    SUBCASE("dependencies first, ties by name") {
        std::vector<pl::Manifest> ms{manifest("game", {{"physics_ext", {1, 0, 0}}, {"ai_ext", {}}}),
                                     manifest("physics_ext", {{"core_utils", {}}}), manifest("core_utils"),
                                     manifest("ai_ext"), manifest("zeta")};
        auto r = pl::resolve(ms);
        CHECK(r.rejected.empty());
        CHECK(names(ms, r.order) == std::vector<std::string>{"ai_ext", "core_utils", "physics_ext", "game", "zeta"});
    }
    SUBCASE("missing and too-old dependencies propagate to dependents") {
        std::vector<pl::Manifest> ms{manifest("a", {{"missing", {}}}), manifest("b", {{"a", {}}}),
                                     manifest("old", {}, "0.9.0"), manifest("c", {{"old", {1, 0, 0}}}),
                                     manifest("fine")};
        auto r = pl::resolve(ms);
        CHECK(names(ms, r.order) == std::vector<std::string>{"fine", "old"});
        CHECK(r.rejected.at("a").find("missing dependency 'missing'") != std::string::npos);
        CHECK(r.rejected.at("b").find("dependency 'a' cannot load") != std::string::npos);
        CHECK(r.rejected.at("c").find("needs >= 1.0.0") != std::string::npos);
    }
    SUBCASE("cycles are rejected with their members, other plugins still load") {
        std::vector<pl::Manifest> ms{manifest("x", {{"y", {}}}), manifest("y", {{"z", {}}}), manifest("z", {{"x", {}}}),
                                     manifest("solo")};
        auto r = pl::resolve(ms);
        CHECK(names(ms, r.order) == std::vector<std::string>{"solo"});
        REQUIRE(r.rejected.size() == 3);
        CHECK(r.rejected.at("y") == "dependency cycle among: x, y, z");
    }
    SUBCASE("duplicates, disabled plugins and API versions") {
        auto dup1 = manifest("dup");
        auto dup2 = manifest("dup");
        auto off = manifest("off");
        off.enabled = false;
        auto needs_off = manifest("needs_off", {{"off", {}}});
        auto future = manifest("future");
        future.api = {1, 7, 0};
        auto major = manifest("major");
        major.api = {2, 0, 0};
        std::vector<pl::Manifest> ms{dup1, dup2, off, needs_off, future, major};
        auto r = pl::resolve(ms, {1, 6, 0});
        CHECK(r.order.empty());
        CHECK(r.rejected.contains("dup"));
        CHECK_FALSE(r.rejected.contains("off"));
        CHECK(r.rejected.contains("needs_off"));
        CHECK(r.rejected.at("future") == "needs plugin API 1.7, host provides 1.6");
        CHECK(r.rejected.contains("major"));
        auto r2 = pl::resolve({future}, {1, 7, 0});
        CHECK(r2.order.size() == 1);
    }
}

TEST_CASE("plugin: a native plugin loads, runs systems, serves functions and shuts down") {
    PluginDir dir("lifecycle");
    dir.add("alpha", VICTUS_TEST_PLUGIN_OK);
    dir.add("beta", VICTUS_TEST_PLUGIN_OK, R"(,"dependencies":{"alpha":"1.0"})");
    LogCapture logs;
    scene::Scene s;
    const scene::Entity mover = s.create("Mover");
    gameplay::Simulation sim;
    {
        pl::PluginManager pm;
        pm.load_directory(dir.root);
        REQUIRE(pm.loaded_count() == 2);
        const auto* alpha = pm.status("alpha");
        REQUIRE(alpha);
        CHECK(alpha->state == pl::State::Loaded);
        CHECK(alpha->systems == std::vector<std::string>{"plugin.alpha.mover"});
        CHECK(alpha->functions.size() == 7);
        CHECK(pm.has_function("beta.greet"));
        CHECK(logs.lines("init") == std::vector<std::string>{"plugin.alpha", "plugin.beta"});

        pm.attach(sim);
        CHECK(sim.find("plugin.alpha.mover") != nullptr);
        CHECK(sim.find("plugin.beta.mover") != nullptr);
        for (int i = 0; i < 3; ++i) sim.tick(s, 0.1f);
        // Both plugins move the entity by dt each frame: 3 frames * 0.1 * 2 plugins.
        CHECK(s.world().get<scene::Transform>(mover)->position.x == doctest::Approx(0.6f));
        const scene::Entity child = s.find_by_name("PluginChild");
        REQUIRE_FALSE(child.is_null());
        CHECK(s.parent(child) == mover);
        CHECK(s.world().get<scene::Transform>(child)->scale.x == doctest::Approx(2.0f));

        CHECK(pm.call("alpha.greet", "null").value() == "\"hello\"");
        CHECK(pm.call("alpha.echo", R"({"a":[1,2]})").value() == R"({"a":[1,2]})");
        CHECK(pm.call("alpha.ticks", "null").value() == "3");
        auto relayed = pm.call("beta.relay", R"("alpha.greet")");
        const std::string relay_error = relayed ? std::string() : relayed.error().message;
        INFO(relay_error);
        REQUIRE(relayed);
        CHECK(*relayed == "\"hello\"");
        CHECK(pm.call("alpha.late_add", "null").value() == "1"); // add_system after init is refused
        auto failed = pm.call("alpha.fail", "{}");
        REQUIRE_FALSE(failed);
        CHECK(failed.error().message.find("nope") != std::string::npos);
        CHECK_FALSE(pm.call("alpha.missing", "null"));
        CHECK_FALSE(pm.call("alpha.echo", "{not json"));
        CHECK_FALSE(pm.call("beta.relay", R"("nobody.here")"));
    }
    // Reverse init order on shutdown; the systems left in the simulation become no-ops.
    CHECK(logs.lines("shutdown") == std::vector<std::string>{"plugin.beta", "plugin.alpha"});
    sim.tick(s, 0.1f);
    CHECK(s.world().get<scene::Transform>(mover)->position.x == doctest::Approx(0.6f));
}

TEST_CASE("plugin: failures are isolated per plugin with readable reasons") {
    PluginDir dir("failures");
    dir.add("good", VICTUS_TEST_PLUGIN_OK);
    dir.add("crashy", VICTUS_TEST_PLUGIN_FAIL_INIT);
    dir.add("needs_crashy", VICTUS_TEST_PLUGIN_OK, R"(,"dependencies":{"crashy":"1.0"})");
    dir.add("newer", VICTUS_TEST_PLUGIN_NEWER_API);
    dir.add("noentry", VICTUS_TEST_PLUGIN_NO_ENTRY);
    dir.add("future_api", VICTUS_TEST_PLUGIN_OK, "", "1.99");
    dir.add("editor_only", VICTUS_TEST_PLUGIN_OK, R"(,"targets":["editor"])");
    dir.add("off", VICTUS_TEST_PLUGIN_OK, R"(,"enabled":false)");
    std::filesystem::create_directories(dir.root / "nolib");
    std::ofstream(dir.root / "nolib" / "nolib.vplugin")
        << R"({"name":"nolib","version":"1.0.0","api":"1.0","libraries":{"other_os":"x.so"}})";
    // A broken manifest is reported by discovery and does not stop the others.
    std::filesystem::create_directories(dir.root / "broken");
    std::ofstream(dir.root / "broken" / "broken.vplugin") << "{";

    std::vector<pl::DiscoveryIssue> issues;
    auto found = pl::discover(dir.root, &issues);
    CHECK(found.size() == 9);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].path.filename() == "broken.vplugin");

    pl::PluginManager pm;
    pm.load_directory(dir.root);
    auto state = [&](std::string_view n) { return pm.status(n) ? pm.status(n)->state : pl::State::Disabled; };
    auto error = [&](std::string_view n) { return pm.status(n) ? pm.status(n)->error : std::string(); };
    CHECK(state("good") == pl::State::Loaded);
    CHECK(pm.loaded_count() == 1);
    CHECK(state("crashy") == pl::State::Failed);
    CHECK(error("crashy") == "init failed: boom");
    CHECK(pm.status("crashy")->systems.empty()); // registrations of a failed init are dropped
    CHECK_FALSE(pm.has_function("crashy.greet"));
    CHECK(state("needs_crashy") == pl::State::Skipped);
    CHECK(state("newer") == pl::State::Failed);
    CHECK(error("newer").find("built against plugin API 1.2") != std::string::npos);
    CHECK(state("noentry") == pl::State::Failed);
    CHECK(error("noentry") == "library does not export 'victus_plugin_entry'");
    CHECK(state("future_api") == pl::State::Rejected);
    CHECK(state("editor_only") == pl::State::Disabled);
    CHECK(error("editor_only") == "not loaded by the player");
    CHECK(state("off") == pl::State::Disabled);
    CHECK(state("nolib") == pl::State::Failed);
    CHECK(error("nolib").find("no library for platform") != std::string::npos);

    gameplay::Simulation sim;
    pm.attach(sim);
    std::size_t plugin_systems = 0;
    for (const auto& sys : sim.systems())
        if (sys.name.starts_with("plugin.")) ++plugin_systems;
    CHECK(plugin_systems == 1);
}

TEST_CASE("plugin: the editor target loads editor plugins and a missing directory is empty") {
    PluginDir dir("editor");
    dir.add("editor_only", VICTUS_TEST_PLUGIN_OK, R"(,"targets":["editor"])");
    pl::PluginManager editor({.target = "editor"});
    editor.load_directory(dir.root);
    CHECK(editor.loaded_count() == 1);
    CHECK(*editor.call("editor_only.has_editor", "null") == "true");
    pl::PluginManager none;
    none.load_directory(dir.root / "does_not_exist");
    CHECK(none.statuses().empty());
}

TEST_CASE("plugin: editor hosts serve victus.editor.1 commands; players do not") {
    PluginDir dir("commands");
    dir.add("tools", VICTUS_TEST_PLUGIN_OK);
    std::vector<std::string> notes;
    pl::HostOptions options{.target = "editor"};
    options.notify = [&](std::string_view plugin, std::string_view message, bool error) {
        notes.push_back(std::format("{}:{}:{}", plugin, message, error));
    };
    pl::PluginManager editor(std::move(options));
    editor.load_directory(dir.root);
    REQUIRE(editor.loaded_count() == 1);
    const auto cmds = editor.commands();
    REQUIRE(cmds.size() == 4); // the duplicate and the invalid name were refused
    CHECK(cmds[0].id == "tools.spawn");
    CHECK(cmds[0].label == "Spawn Plugin Cube");
    CHECK(cmds[0].plugin == "tools");
    CHECK(cmds[3].label == "broken"); // empty label falls back to the name
    CHECK(editor.status("tools")->commands.size() == 4);

    scene::Scene s;
    REQUIRE(editor.run_command("tools.spawn", s).ok());
    const auto cube = s.find_by_name("PluginCube");
    REQUIRE_FALSE(cube.is_null());
    CHECK(s.world().get<scene::Transform>(cube)->position.y == doctest::Approx(2.0f));
    CHECK(s.children(cube).size() == 1);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0] == "tools:spawned:false");
    REQUIRE(editor.run_command("tools.clear", s).ok());
    CHECK(s.find_by_name("PluginCube").is_null());
    auto broken = editor.run_command("tools.broken", s);
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message == "broken failed: nothing selected");
    CHECK_FALSE(editor.run_command("tools.missing", s).ok());

    // The same library in a player host gets no editor interface and registers no commands.
    pl::PluginManager player({.target = "player"});
    PluginDir pdir("commands_player");
    pdir.add("tools", VICTUS_TEST_PLUGIN_OK);
    player.load_directory(pdir.root);
    REQUIRE(player.loaded_count() == 1);
    CHECK(player.commands().empty());
    CHECK(*player.call("tools.has_editor", "null") == "false");
    player.shutdown();

    // After shutdown the commands are gone.
    editor.shutdown();
    CHECK(editor.commands().empty());
    CHECK_FALSE(editor.run_command("tools.spawn", s).ok());
}

TEST_CASE("plugin: scaffold writes a buildable template and set_enabled edits only that field") {
    PluginDir dir("scaffold");
    REQUIRE(pl::scaffold(dir.root, "hello").ok());
    CHECK_FALSE(pl::scaffold(dir.root, "hello").ok()); // exists
    CHECK_FALSE(pl::scaffold(dir.root, "Bad Name").ok());
    const fs::Path m = dir.root / "hello" / "hello.vplugin";
    auto parsed = pl::read_manifest(m);
    REQUIRE(parsed.ok());
    CHECK(parsed->name == "hello");
    CHECK(parsed->api == pl::host_api_version());
    CHECK(parsed->enabled);
    auto c = fs::read_text(dir.root / "hello" / "src" / "hello.c");
    REQUIRE(c.ok());
    CHECK(c->find("VICTUS_EDITOR_INTERFACE_1") != std::string::npos);
    CHECK(c->find("add_spinner") != std::string::npos);
    auto sdk = fs::read_text(dir.root / "hello" / "include" / "victus" / "plugin" / "victus_plugin.h");
    REQUIRE(sdk.ok());
    CHECK(sdk->find("VictusEditorV1") != std::string::npos);
    REQUIRE(pl::set_enabled(m, false).ok());
    parsed = pl::read_manifest(m);
    REQUIRE(parsed.ok());
    CHECK_FALSE(parsed->enabled);
    CHECK(parsed->libraries.size() == 3);
    CHECK(parsed->targets.size() == 2);
    CHECK_FALSE(pl::set_enabled(dir.root / "nope.vplugin", true).ok());
}
