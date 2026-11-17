// SPDX-License-Identifier: Apache-2.0
// Procedural meshes from scripts (entity:set_mesh_data), json.encode/decode and bridges to helper
// processes speaking JSON lines (bridge.open).
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <cstring>
#include <filesystem>

using namespace victus;
using scene::Entity;
using scene::Scene;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path / "scripts");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& text) const { REQUIRE(fs::write_text_atomic(path / rel, text).ok()); }
};

std::unique_ptr<script::ScriptHost> make_host(const TempDir& d, script::HostOptions o = {}) {
    o.assets_root = d.path;
    auto h = script::ScriptHost::create(std::move(o));
    REQUIRE(h.ok());
    return std::move(h).value();
}

Entity scripted(Scene& s, const char* name, const char* path, json::Value props = json::Value::object()) {
    const Entity e = s.create(name);
    auto& c = s.world().add<script::ScriptComponent>(e);
    c.script = path;
    c.properties = std::move(props);
    return e;
}

// One 20-byte vertex: float32 xyz, int8 normal xyzw (/127), uint8 rgba.
void pack_vertex(std::vector<std::uint8_t>& out, float x, float y, float z, std::int8_t nx, std::int8_t ny, std::int8_t nz,
                 std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    const float p[3] = {x, y, z};
    const auto at = out.size();
    out.resize(at + 20);
    std::memcpy(out.data() + at, p, 12);
    out[at + 12] = static_cast<std::uint8_t>(nx);
    out[at + 13] = static_cast<std::uint8_t>(ny);
    out[at + 14] = static_cast<std::uint8_t>(nz);
    out[at + 15] = 0;
    out[at + 16] = r;
    out[at + 17] = g;
    out[at + 18] = b;
    out[at + 19] = a;
}

} // namespace

TEST_CASE("scene: publish_mesh validates, bounds and smooth normals") {
    scene::ProceduralMesh m;
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};
    m.indices = {0, 1, 2};
    auto a = scene::publish_mesh(m);
    REQUIRE(a);
    CHECK((*a)->id != 0);
    CHECK((*a)->normals.size() == 3);
    CHECK((*a)->normals[0].y == doctest::Approx(1.0f)); // counter-clockwise seen from above
    CHECK((*a)->bounds.max.x == doctest::Approx(1.0f));
    CHECK((*a)->bounds.min.z == doctest::Approx(-1.0f));
    auto b = scene::publish_mesh(m);
    REQUIRE(b);
    CHECK((*b)->id != (*a)->id); // ids are never reused
    m.indices = {0, 1, 3};
    CHECK_FALSE(scene::publish_mesh(m));
    m.indices = {0, 1};
    CHECK_FALSE(scene::publish_mesh(m));
    m.indices = {0, 1, 2};
    m.colors = {1u};
    CHECK_FALSE(scene::publish_mesh(m));
}

TEST_CASE("script: set_mesh_data builds procedural meshes from packed bytes and tables") {
    TempDir d("victus_script_procmesh");
    script::register_components();
    std::vector<std::uint8_t> vb;
    pack_vertex(vb, 0, 0, 0, 0, 127, 0, 255, 0, 0, 255);
    pack_vertex(vb, 2, 0, 0, 0, 127, 0, 0, 255, 0, 0);
    pack_vertex(vb, 0, 0, 2, 0, 127, 0, 0, 0, 255, 128);
    const std::uint32_t idx[3] = {0, 2, 1}; // clockwise from above, as a left-handed engine writes it
    std::vector<std::uint8_t> ib(12);
    std::memcpy(ib.data(), idx, 12);
    json::Value props = json::Value::object();
    props.set("vertices", base64_encode(vb));
    props.set("indices", base64_encode(ib));
    d.write("scripts/mesh.luau", R"lua(
local M = {}
M.vertices = ""
M.indices = ""
function M:start()
    local layout = "position:f32x3,normal:s8x4,color:u8x4"
    local v, t = self.entity:set_mesh_data({vertices = self.vertices, indices = self.indices, layout = layout,
                                            scale = vector.create(1, 1, -1)})
    assert(v == 3 and t == 1, "packed counts")
    local info = self.entity:mesh_data()
    assert(info.vertices == 3 and info.triangles == 1, "info")
    assert(info.min.z == -2 and info.max.x == 2, "mirrored bounds")
    -- binary encoding through string.pack
    local raw = string.pack("<fff", 0, 0, 0) .. string.pack("<fff", 1, 0, 0) .. string.pack("<fff", 0, 1, 0)
    local tri = string.pack("<HHH", 0, 1, 2)
    local other = scene.spawn("Raw")
    other:set_mesh_data({vertices = raw, indices = tri, layout = "position:f32x3", index = "u16", encoding = "binary"})
    assert(other:mesh_data().triangles == 1, "binary")
    local tbl = scene.spawn("Table")
    tbl:set_mesh_data({positions = {vector.create(0, 0, 0), vector.create(1, 0, 0), vector.create(0, 0, -1)},
                       indices = {1, 2, 3}, colors = {vector.create(1, 0, 0), vector.create(0, 1, 0), vector.create(0, 0, 1)}})
    assert(tbl:mesh_data().vertices == 3, "table form")
    local function fails(spec, needle)
        local ok, err = pcall(tbl.set_mesh_data, tbl, spec)
        assert(not ok and string.find(err, needle, 1, true), needle .. " expected, got " .. tostring(err))
    end
    fails({vertices = raw, indices = tri, layout = "position:f32x3", index = "u32", encoding = "binary"}, "multiple of 4")
    fails({vertices = raw, indices = string.pack("<HHH", 0, 1, 9), layout = "position:f32x3", index = "u16", encoding = "binary"}, "out of range")
    fails({vertices = "!!", indices = "", layout = "position:f32x3"}, "not valid base64")
    fails({vertices = raw, indices = tri, layout = "normal:f32x3", encoding = "binary"}, "no position")
    fails({vertices = raw, indices = tri, layout = "position:f32x3,colour:u8x4", encoding = "binary"}, "unknown attribute")
    fails({vertices = raw .. "x", indices = tri, layout = "position:f32x3", index = "u16", encoding = "binary"}, "not a multiple")
    fails({positions = {vector.create(0, 0, 0)}, indices = {0}}, "1-based")
    other:clear_mesh_data()
    assert(other:mesh_data() == nil, "cleared")
end
return M
)lua");
    Scene s;
    const Entity e = scripted(s, "Terrain", "scripts/mesh.luau", props);
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    sim.tick(s, 0.02f);
    for (const auto& err : host->recent_errors()) MESSAGE(err);
    CHECK(host->stats().errors == 0);
    const auto* pm = s.world().get<scene::ProceduralMeshComponent>(e);
    REQUIRE(pm);
    REQUIRE(pm->mesh);
    const scene::ProceduralMesh& m = *pm->mesh;
    REQUIRE(m.positions.size() == 3);
    CHECK(m.positions[2].z == doctest::Approx(-2.0f)); // mirrored on Z
    CHECK(m.normals[0].y == doctest::Approx(1.0f));
    // Mirroring flips the winding back to counter-clockwise: 0, 1, 2.
    CHECK(m.indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(m.colors[0] == 0xFF0000FFu);
    CHECK(m.colors[2] == 0x80FF0000u); // alpha byte kept (coverage masks)
    CHECK(s.world().get<scene::MeshRendererComponent>(e) != nullptr); // added on demand
    const Entity raw = s.find_by_name("Raw");
    REQUIRE(raw != Entity{});
    CHECK_FALSE(s.world().get<scene::ProceduralMeshComponent>(raw)); // cleared
    const Entity table = s.find_by_name("Table");
    const auto* tm = s.world().get<scene::ProceduralMeshComponent>(table);
    REQUIRE(tm);
    CHECK((tm->mesh->colors[1] & 0x00FF00u) == 0x00FF00u);
    CHECK(tm->mesh->normals[0].y == doctest::Approx(1.0f)); // computed
}

TEST_CASE("script: json.encode and json.decode") {
    TempDir d("victus_script_json");
    auto host = make_host(d);
    auto r = host->eval(R"lua(
local t = json.decode('{"a": [1, 2, 3, 4], "b": {"c": "x"}, "n": null}')
assert(#t.a == 4 and t.b.c == "x" and t.n == nil, "decode")
local s = json.encode({op = "new", seed = 7, eye = vector.create(1, 2, 3)})
local back = json.decode(s)
assert(back.op == "new" and back.seed == 7 and back.eye.y == 2, "round trip " .. s)
assert(not pcall(json.decode, "{oops"), "bad json")
return s
)lua");
    REQUIRE(r);
    CHECK(r->as_string().find("\"seed\":7") != std::string::npos); // integers stay integers on the wire
}

TEST_CASE("script: bridges are unavailable without a launcher") {
    TempDir d("victus_script_nobridge");
    auto host = make_host(d);
    auto r = host->eval(R"lua(
local b, err = bridge.open("sim")
assert(b == nil and string.find(err, "not available"), tostring(err))
return true
)lua");
    REQUIRE(r);
}

#if !defined(_WIN32)
TEST_CASE("script: bridge requests and replies over JSON lines (POSIX cat echo)") {
    TempDir d("victus_script_bridge");
    script::HostOptions o;
    int launches = 0;
    o.bridge_launcher = [&launches](std::string_view name) -> Result<process::LaunchOptions> {
        ++launches;
        if (name != "echo") return make_error(ErrorCode::NotFound, "no bridge named " + std::string(name));
        process::LaunchOptions lo;
        lo.args = {"cat"};
        return lo;
    };
    auto host = make_host(d, std::move(o));
    auto r = host->eval(R"lua(
local missing, err = bridge.open("nope")
assert(missing == nil and string.find(err, "no bridge named"), tostring(err))
local b = assert(bridge.open("echo"))
assert(typeof(b) == "Bridge", typeof(b))
assert(b:running(), "running")
local reply = b:request({op = "hello", n = 3})
assert(reply.op == "hello" and reply.n == 3 and reply.id == 1, "echoed with id")
-- Messages that arrive while waiting for a reply are kept for receive().
local early = b:send({op = "note"})
local second = b:request({op = "frame", dt = 0.5})
assert(second.op == "frame" and second.dt == 0.5, "second reply")
local note = b:receive(1)
assert(note and note.op == "note" and note.id == early, "queued message")
assert(b:receive() == nil, "nothing else")
assert(bridge.open("echo") ~= nil, "reopen returns the running helper")
local st = b:stats()
assert(st.name == "echo" and st.requests == 2 and st.wait_seconds >= 0, "stats")
b:close()
assert(not b:running(), "closed")
assert(not pcall(b.request, b, {op = "x"}), "closed bridges refuse requests")
return true
)lua");
    if (!r) MESSAGE(r.error().message);
    REQUIRE(r);
    CHECK(launches == 2); // "nope" and "echo"; the reopen reused the running process
}

TEST_CASE("script: a bridge that exits reports it") {
    TempDir d("victus_script_bridge_exit");
    script::HostOptions o;
    o.bridge_launcher = [](std::string_view) -> Result<process::LaunchOptions> {
        process::LaunchOptions lo;
        lo.args = {"/bin/sh", "-c", "read line; exit 3"};
        return lo;
    };
    auto host = make_host(d, std::move(o));
    auto r = host->eval(R"lua(
local b = assert(bridge.open("x"))
local ok, err = pcall(b.request, b, {op = "hello"})
assert(not ok and string.find(err, "stopped"), tostring(err))
return true
)lua");
    if (!r) MESSAGE(r.error().message);
    REQUIRE(r);
}
#endif

TEST_CASE("script: require shares library modules between scripts") {
    TempDir d("victus_script_require");
    script::register_components();
    d.write("scripts/lib/counter.luau", R"lua(
local C = {count = 0}
function C.bump() C.count += 1; return C.count end
return C
)lua");
    d.write("scripts/lib/a.luau", "local b = require(\"scripts/lib/b\")\nreturn {}\n");
    d.write("scripts/lib/b.luau", "local a = require(\"scripts/lib/a\")\nreturn {}\n");
    d.write("scripts/user.luau", R"lua(
local counter = require("scripts/lib/counter")
local M = {}
function M:start()
    self.value = counter.bump()
    assert(require("scripts/lib/counter.luau") == counter, "same table")
    local ok, err = pcall(require, "scripts/lib/missing")
    assert(not ok and string.find(err, "cannot load"), tostring(err))
    ok, err = pcall(require, "scripts/lib/a")
    assert(not ok, "circular require fails")
    self.entity.name = "bumped" .. self.value
end
return M
)lua");
    Scene s;
    const Entity a = scripted(s, "A", "scripts/user.luau");
    const Entity b = scripted(s, "B", "scripts/user.luau");
    auto host = make_host(d);
    gameplay::Simulation sim(1.0f / 50.0f);
    script::add_script_systems(sim, *host);
    sim.tick(s, 0.02f);
    const std::string na = s.world().get<scene::Name>(a)->value, nb = s.world().get<scene::Name>(b)->value;
    CHECK(((na == "bumped1" && nb == "bumped2") || (na == "bumped2" && nb == "bumped1")));
    // The missing module and the cycle were reported; nothing else failed.
    for (const auto& err : host->recent_errors()) MESSAGE(err);
}

TEST_CASE("script: input.mouse_wheel reports this frame's notches") {
    TempDir d("victus_script_wheel");
    script::register_components();
    d.write("scripts/zoom.luau", R"lua(
local M = {}
M.zoom = 10
function M:update(dt)
    self.zoom -= input.mouse_wheel() * 2
    self.entity.name = "zoom" .. self.zoom
end
return M
)lua");
    Scene s;
    const Entity e = scripted(s, "Camera", "scripts/zoom.luau");
    auto host = make_host(d);
    gameplay::ManualInput in;
    in.add_mouse_wheel(1.5f);
    host->update(s, gameplay::FrameTime{0.0, 0.02f, 1}, &in);
    CHECK(s.world().get<scene::Name>(e)->value == "zoom7");
    in.end_frame();
    host->update(s, gameplay::FrameTime{0.02, 0.02f, 2}, &in);
    CHECK(s.world().get<scene::Name>(e)->value == "zoom7"); // cleared at the end of the frame
}

TEST_CASE("script: input.touch reports fingers in touch order") {
    TempDir d("victus_script_touch");
    script::register_components();
    d.write("scripts/touch.luau", R"lua(
local M = {}
function M:update(dt)
    local n = input.touch_count()
    local p, id = input.touch(1)
    local missing = input.touch(n + 1) == nil
    if p then
        self.entity.name = string.format("%d:%.2f,%.2f,%.1f:%d:%s", n, p.x, p.y, p.z, id, tostring(missing))
    else
        self.entity.name = string.format("%d:none:%s", n, tostring(missing))
    end
end
return M
)lua");
    Scene s;
    const Entity e = scripted(s, "Pad", "scripts/touch.luau");
    auto host = make_host(d);
    gameplay::ManualInput in;
    in.set_touches({{7, {0.25f, 0.75f}, 0.5f}, {9, {0.9f, 0.1f}, 1.0f}});
    host->update(s, gameplay::FrameTime{0.0, 0.02f, 1}, &in);
    CHECK(s.world().get<scene::Name>(e)->value == "2:0.25,0.75,0.5:7:true");
    in.set_touches({});
    host->update(s, gameplay::FrameTime{0.02, 0.02f, 2}, &in);
    CHECK(s.world().get<scene::Name>(e)->value == "0:none:true");
    for (const auto& err : host->recent_errors()) MESSAGE(err);
    CHECK(host->recent_errors().empty());
}
