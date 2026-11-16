// SPDX-License-Identifier: Apache-2.0
// Networking tests (Slice 17): wire format bounds, link emulation, the in-memory and ENet UDP
// transports, and full server/client sessions — handshake and validation, player templates,
// input routing, client-side prediction and reconciliation, interpolated proxies, network
// variables, script messages, despawn on disconnect, snapshot budgets and malformed traffic,
// (Slice 23) delta snapshots against acknowledged baselines and (Slice 26) encrypted,
// authenticated sessions.
#include "victus/net/net.hpp"
#include "victus/net/wire.hpp"

#include "victus/core/fs.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/script/script.hpp"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace victus;
using namespace victus::net;
using scene::Scene;
using scene::Transform;

namespace {

constexpr float kDt = 1.0f / 60.0f;

float horizontal(math::Vec3 a, math::Vec3 b) { return std::hypot(a.x - b.x, a.z - b.z); }

// ---- scene + scripts shared by the session tests ----

constexpr const char* kPlayerScript = R"lua(
local P = {}
P.speed = 4
function P:update(dt)
    if net.authority() or self.entity:is_local() then
        local stick = input.vector("move_x", "move_y") -- keys or a gamepad's left stick
        self.entity:move_character(vector.create(stick.x, 0, -stick.y) * self.speed)
        if net.authority() and input.action_pressed("jump") then self.entity:set_net("jumps", (self.entity:get_net("jumps") or 0) + 1) end
    end
end
return P
)lua";

constexpr const char* kRelayScript = R"lua(
local R = {}
function R:start() self.count = 0 end
function R:on_net_message(name, value, sender)
    if net.is_server() and name == "ping" then
        net.send("pong", value * 2, sender)
    elseif name == "pong" then
        self.entity.name = "pong " .. tostring(value)
    elseif net.is_server() and name == "client_joined" then
        self.count += 1
        self.entity:set_net("joined", self.count)
    end
end
return R
)lua";

constexpr const char* kStatusScript = R"lua(
local S = {}
function S:update(dt)
    local s = net.stats()
    self.entity.name = net.role() .. ":" .. tostring(net.authority()) .. ":" .. tostring(#net.clients())
end
return S
)lua";

fs::Path script_dir() {
    static const fs::Path dir = [] {
        const fs::Path d = std::filesystem::temp_directory_path() / "victus_net_tests";
        std::filesystem::create_directories(d / "scripts");
        auto put = [&](const char* name, const char* text) {
            std::ofstream(d / "scripts" / name, std::ios::binary) << text;
        };
        put("player.luau", kPlayerScript);
        put("relay.luau", kRelayScript);
        put("status.luau", kStatusScript);
        return d;
    }();
    return dir;
}

void attach(Scene& s, Entity e, const char* path) {
    auto& c = s.world().add<script::ScriptComponent>(e);
    c.script = path;
}

// Floor, a player template, three replicated crates and a relay entity.
void build_arena(Scene& s, int extra_networked = 0) {
    const Entity floor = s.create("Floor");
    s.world().get<Transform>(floor)->position = {0, -0.5f, 0};
    s.world().add<physics::RigidBodyComponent>(floor).type = physics::BodyType::Static;
    s.world().add<physics::ColliderComponent>(floor).size = {60, 1, 60};

    const Entity player = s.create("Player");
    s.world().get<Transform>(player)->position = {0, 0, 0};
    s.world().add<NetworkIdentityComponent>(player).player_template = true;
    auto& cc = s.world().add<physics::CharacterControllerComponent>(player);
    cc.radius = 0.4f;
    cc.height = 1.6f;
    attach(s, player, "scripts/player.luau");
    s.create("Badge", player);

    for (int i = 0; i < 3; ++i) {
        const Entity crate = s.create("Crate " + std::to_string(i));
        s.world().get<Transform>(crate)->position = {6.0f + 1.5f * static_cast<float>(i), 0.5f, -6.0f};
        s.world().add<physics::RigidBodyComponent>(crate).mass = 2.0f;
        s.world().add<physics::ColliderComponent>(crate);
        s.world().add<NetworkIdentityComponent>(crate);
    }
    for (int i = 0; i < extra_networked; ++i) {
        const Entity e = s.create("Marker " + std::to_string(i));
        s.world().get<Transform>(e)->position = {static_cast<float>(i % 20) - 10.0f, 3.0f, static_cast<float>(i / 20) - 10.0f};
        s.world().add<NetworkIdentityComponent>(e);
        s.world().add<gameplay::RotatorComponent>(e) = {{0, 1, 0}, 90.0f};
    }
    attach(s, s.create("Relay"), "scripts/relay.luau");
    attach(s, s.create("Status"), "scripts/status.luau");
    s.update_transforms();
}

// Every machine loads the same scene file, as a game would, so scene objects share UUIDs.
fs::Path arena_file(int extra) {
    const fs::Path f = script_dir() / ("arena_" + std::to_string(extra) + ".vscene");
    if (!std::filesystem::exists(f)) {
        Scene s;
        build_arena(s, extra);
        REQUIRE(s.save_file(f).ok());
    }
    return f;
}

// One machine: scene, scripts, physics and simulation (server or client attached later).
struct Peer {
    Scene scene;
    std::unique_ptr<script::ScriptHost> host;
    std::unique_ptr<physics::PhysicsWorld> physics_world;
    gameplay::ManualInput input;
    gameplay::Simulation sim;

    explicit Peer(int extra = 0) {
        physics::register_components();
        script::register_components();
        net::register_components();
        gameplay::register_components();
        REQUIRE(scene.load_file(arena_file(extra)).ok());
        script::HostOptions so;
        so.assets_root = script_dir();
        host = std::move(script::ScriptHost::create(so)).value();
        physics_world = std::move(physics::PhysicsWorld::create()).value();
        gameplay::add_builtin_systems(sim);
        script::add_script_systems(sim, *host);
        physics::add_physics_systems(sim, *physics_world);
        sim.input = &input;
    }
    void tick() {
        sim.tick(scene, kDt);
        input.end_frame();
    }
    Entity find(std::string_view name) { return scene.find_by_name(name); }
    math::Vec3 position(Entity e) {
        const auto* t = scene.world().get<Transform>(e);
        return t ? t->position : math::Vec3{};
    }
};

ClientOptions client_options(std::uint16_t port, const char* name) {
    ClientOptions o;
    o.host = "memory";
    o.port = port;
    o.name = name;
    o.game_id = "arena";
    return o;
}

// A server and clients on one in-memory network, stepped in lock-step at 60 Hz.
struct Session {
    MemoryNetwork network;
    Peer server_peer;
    std::unique_ptr<NetServer> server;
    std::vector<std::unique_ptr<Peer>> peers;
    std::vector<std::unique_ptr<NetClient>> clients;

    int extra;
    explicit Session(ServerOptions so = {}, int extra_ = 0) : server_peer(extra_), extra(extra_) {
        so.game_id = so.game_id.empty() ? "arena" : so.game_id;
        server = std::make_unique<NetServer>(std::move(network.listen(7777)).value(), so);
        server->install(server_peer.sim);
    }
    NetClient& add_client(const char* name, LinkConditions link = {}, std::string game_id = "arena") {
        peers.push_back(std::make_unique<Peer>(extra));
        std::unique_ptr<Transport> t = network.endpoint();
        if (link.active()) t = with_conditions(std::move(t), link);
        auto o = client_options(7777, name);
        o.game_id = std::move(game_id);
        clients.push_back(std::make_unique<NetClient>(std::move(t), o));
        clients.back()->install(peers.back()->sim);
        return *clients.back();
    }
    void step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            server_peer.tick();
            for (auto& p : peers) p->tick();
        }
    }
    // Server-side player entity of client `id`.
    Entity server_player(std::uint16_t id) {
        for (const auto& c : server->clients())
            if (c.id == id) return c.player;
        return {};
    }
};

} // namespace

// ---- wire format -----------------------------------------------------------------------------

TEST_CASE("wire: values round-trip and reads past the end fail without overrunning") {
    ByteWriter w;
    w.u8(7);
    w.u16(0xBEEF);
    w.u32(123456789u);
    w.varuint(300);
    w.varuint(0xFFFFFFFFFFull);
    w.f32(1.5f);
    w.str("hello");
    w.vec3({1.25f, -2.5f, 3.0f});
    w.quat(math::Quat::axis_angle(math::normalize(math::Vec3{1, 2, 3}), 0.7f));
    ByteReader r(w.data());
    CHECK(r.u8() == 7);
    CHECK(r.u16() == 0xBEEF);
    CHECK(r.u32() == 123456789u);
    CHECK(r.varuint() == 300);
    CHECK(r.varuint() == 0xFFFFFFFFFFull);
    CHECK(r.f32() == 1.5f);
    CHECK(r.str(64) == "hello");
    const math::Vec3 v = r.vec3();
    CHECK(v.x == 1.25f);
    CHECK(v.y == -2.5f);
    const math::Quat q = r.quat();
    const math::Quat e = math::Quat::axis_angle(math::normalize(math::Vec3{1, 2, 3}), 0.7f);
    CHECK(std::abs(std::abs(q.x * e.x + q.y * e.y + q.z * e.z + q.w * e.w) - 1.0f) < 1e-3f);
    CHECK(r.done());

    // Truncated buffers and oversized strings fail cleanly.
    std::vector<std::uint8_t> bytes = w.data();
    bytes.resize(6);
    ByteReader t(bytes);
    (void)t.u8();
    (void)t.u16();
    (void)t.u32(); // only 3 bytes left
    CHECK(t.failed());
    CHECK(t.u64() == 0); // stays failed, returns zero
    ByteReader s(w.data());
    s.u8(), s.u16(), s.u32(), s.varuint(), s.varuint(), s.f32();
    CHECK(s.str(3).empty()); // "hello" is longer than allowed
    CHECK(s.failed());
    // A varuint that never terminates is rejected.
    const std::vector<std::uint8_t> endless(16, 0xFF);
    ByteReader u(endless);
    (void)u.varuint();
    CHECK(u.failed());
}

// ---- link emulation and transports --------------------------------------------------------------

TEST_CASE("conditions: parse, latency, deterministic loss, reliable delivery under loss") {
    auto c = parse_conditions("latency=50,jitter=10,loss=0.25,bandwidth=0,seed=9");
    REQUIRE(c.ok());
    CHECK(c->latency_ms == 50.0f);
    CHECK(c->jitter_ms == 10.0f);
    CHECK(c->loss == 0.25f);
    CHECK(c->seed == 9u);
    CHECK_FALSE(parse_conditions("latency=fast").ok());
    CHECK_FALSE(parse_conditions("loss=2").ok());
    CHECK_FALSE(parse_conditions("warp=1").ok());

    auto run = [](std::uint64_t seed) {
        MemoryNetwork net;
        auto server = std::move(net.listen(1)).value();
        LinkConditions lc;
        lc.latency_ms = 50;
        lc.loss = 0.25f;
        lc.seed = seed;
        auto client = with_conditions(net.endpoint(), lc);
        const PeerId sp = client->connect("memory", 1).value();
        std::vector<TransportEvent> ev, cev;
        double now = 0;
        PeerId cp = 0;
        for (int i = 0; i < 30; ++i, now += 0.01) {
            client->poll(now, cev);
            server->poll(now, ev);
        }
        for (const auto& e : ev)
            if (e.type == TransportEvent::Type::Connected) cp = e.peer;
        ev.clear();
        std::vector<int> unreliable, reliable;
        double first_arrival = -1;
        const double sent_at = now - 0.01; // sends are stamped with the client's last poll time
        for (int i = 0; i < 100; ++i) {
            const std::uint8_t b = static_cast<std::uint8_t>(i);
            client->send(sp, Channel::Unreliable, std::span(&b, 1));
            client->send(sp, Channel::Reliable, std::span(&b, 1));
        }
        for (int i = 0; i < 400; ++i, now += 0.005) {
            client->poll(now, cev);
            server->poll(now, ev);
            for (const auto& e : ev) {
                if (e.type != TransportEvent::Type::Received) continue;
                if (first_arrival < 0) first_arrival = now;
                (e.channel == Channel::Reliable ? reliable : unreliable).push_back(e.data[0]);
            }
            ev.clear();
        }
        CHECK(cp != 0);
        CHECK(first_arrival - sent_at >= 0.045); // the 50 ms latency is applied
        return std::make_pair(unreliable, reliable);
    };
    const auto a = run(42), b = run(42), d = run(43);
    CHECK(a.first == b.first); // same seed, same drops
    CHECK(a.first != d.first);
    CHECK(a.first.size() > 55);
    CHECK(a.first.size() < 95);
    REQUIRE(a.second.size() == 100); // reliable: never lost, in order
    for (int i = 0; i < 100; ++i) CHECK(a.second[static_cast<std::size_t>(i)] == i);
}

TEST_CASE("memory transport: connect, exchange, disconnect, unknown port") {
    MemoryNetwork net;
    auto server = std::move(net.listen(5)).value();
    CHECK_FALSE(net.listen(5).ok()); // port taken
    auto client = net.endpoint();
    const PeerId sp = client->connect("memory", 5).value();
    std::vector<TransportEvent> se, ce;
    client->poll(0, ce);
    server->poll(0, se);
    client->poll(0, ce);
    REQUIRE(se.size() == 1);
    CHECK(se[0].type == TransportEvent::Type::Connected);
    REQUIRE(ce.size() == 1);
    CHECK(ce[0].type == TransportEvent::Type::Connected);
    const PeerId cp = se[0].peer;
    se.clear(), ce.clear();
    const std::uint8_t hi[2] = {'h', 'i'};
    client->send(sp, Channel::Reliable, hi);
    server->send(cp, Channel::Unreliable, hi);
    client->poll(0, ce);
    server->poll(0, se);
    client->poll(0, ce);
    REQUIRE(se.size() == 1);
    CHECK(se[0].data.size() == 2);
    REQUIRE(ce.size() == 1);
    CHECK(ce[0].channel == Channel::Unreliable);
    se.clear(), ce.clear();
    client->disconnect(sp);
    client->poll(0, ce);
    server->poll(0, se);
    REQUIRE(se.size() == 1);
    CHECK(se[0].type == TransportEvent::Type::Disconnected);

    auto lonely = net.endpoint();
    (void)lonely->connect("memory", 99);
    std::vector<TransportEvent> le;
    lonely->poll(0, le);
    lonely->poll(0, le);
    REQUIRE_FALSE(le.empty());
    CHECK(le.back().type == TransportEvent::Type::Disconnected);
}

TEST_CASE("udp transport: ENet loopback with reliable and unreliable channels") {
    const auto port = static_cast<std::uint16_t>(41000 + (std::chrono::steady_clock::now().time_since_epoch().count() % 2000));
    UdpOptions so;
    so.port = port;
    so.bind_address = "127.0.0.1";
    auto server = create_udp_transport(so);
    REQUIRE(server.ok());
    auto client = create_udp_transport({});
    REQUIRE(client.ok());
    CHECK((*server)->name() == "udp");
    const PeerId sp = (*client)->connect("127.0.0.1", port).value();
    std::vector<TransportEvent> se, ce;
    PeerId cp = 0;
    bool client_connected = false;
    std::vector<std::uint8_t> big(5000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 7);
    bool sent = false;
    int reliable = 0, unreliable = 0;
    bool big_ok = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !(big_ok && reliable == 10 && unreliable > 0)) {
        (*client)->poll(0, ce);
        (*server)->poll(0, se);
        for (const auto& e : se) {
            if (e.type == TransportEvent::Type::Connected) cp = e.peer;
            if (e.type == TransportEvent::Type::Received) {
                if (e.data.size() == big.size()) big_ok = e.data == big;
                else if (e.channel == Channel::Reliable) ++reliable;
                else ++unreliable;
            }
        }
        for (const auto& e : ce)
            if (e.type == TransportEvent::Type::Connected) client_connected = true;
        se.clear(), ce.clear();
        if (client_connected && !sent) {
            sent = true;
            for (std::uint8_t i = 0; i < 10; ++i) {
                (*client)->send(sp, Channel::Reliable, std::span(&i, 1));
                (*client)->send(sp, Channel::Unreliable, std::span(&i, 1));
            }
            (*client)->send(sp, Channel::Reliable, big); // fragmented by ENet
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(cp != 0);
    CHECK(client_connected);
    CHECK(reliable == 10);
    CHECK(unreliable > 0);
    CHECK(big_ok);
    CHECK((*client)->stats(sp).bytes_sent > 5000);
    CHECK_FALSE(parse_address("example.org:99999").ok());
    const auto addr = parse_address("10.0.0.2");
    REQUIRE(addr.ok());
    CHECK(addr->second == kDefaultPort);
}

// ---- sessions ---------------------------------------------------------------------------------

TEST_CASE("session: offline scripts see an authoritative local world") {
    Peer p;
    p.tick();
    CHECK(role_of(p.scene) == Role::Offline);
    CHECK(p.find("offline:true:0") != scene::kNullEntity);
    const Entity player = p.find("Player"); // the template is the player when offline
    REQUIRE(player != scene::kNullEntity);
    CHECK(has_authority(p.scene, player));
    CHECK(is_local(p.scene, player));
    REQUIRE(set_var(p.scene, player, "score", json::Value(3.0)).ok());
    CHECK(get_var(p.scene, player, "score").as_number() == 3.0);
    CHECK_FALSE(set_var(p.scene, player, std::string(100, 'k'), json::Value(1.0)).ok());
    CHECK_FALSE(set_var(p.scene, player, "big", json::Value(std::string(kMaxVarBytes + 1, 'x'))).ok());
    p.input.set_key("D", true);
    for (int i = 0; i < 30; ++i) p.tick();
    CHECK(p.position(player).x > 1.5f);
}

TEST_CASE("session: two clients over a lossy, delayed link play together") {
    Session s;
    LinkConditions lossy;
    lossy.latency_ms = 40;
    lossy.jitter_ms = 10;
    lossy.loss = 0.05f;
    lossy.seed = 7;
    NetClient& a = s.add_client("alice", lossy);
    NetClient& b = s.add_client("bob");
    s.step(60);
    REQUIRE(a.connected());
    REQUIRE(b.connected());
    CHECK(a.local_id() != b.local_id());
    REQUIRE(s.server->clients().size() == 2);
    CHECK(s.server_peer.find("Player") == scene::kNullEntity); // the template is parked
    CHECK(s.server_peer.find("server:true:2") != scene::kNullEntity);

    Peer& pa = *s.peers[0];
    Peer& pb = *s.peers[1];
    CHECK(pa.find("client:false:2") != scene::kNullEntity);
    CHECK(a.clients().size() == 2);
    CHECK(b.clients().size() == 2);
    const Entity a_local = a.local_player();
    const Entity b_local = b.local_player();
    REQUIRE(!a_local.is_null());
    REQUIRE(!b_local.is_null());
    CHECK(is_local(pa.scene, a_local));
    CHECK_FALSE(has_authority(pa.scene, a_local));
    // Each client sees the other's player as a proxy.
    Entity a_view_of_b, b_view_of_a;
    for (const auto& c : a.clients())
        if (c.id == b.local_id()) a_view_of_b = c.player;
    for (const auto& c : b.clients())
        if (c.id == a.local_id()) b_view_of_a = c.player;
    REQUIRE(!a_view_of_b.is_null());
    REQUIRE(!b_view_of_a.is_null());
    CHECK_FALSE(is_local(pa.scene, a_view_of_b));
    REQUIRE(pa.scene.children(a_local).size() == 1); // the hierarchy came along
    CHECK(pa.scene.world().get<scene::Name>(pa.scene.children(a_local)[0])->value == "Badge");
    CHECK(pa.find("Player") == scene::kNullEntity);

    const Entity srv_a = s.server_player(a.local_id());
    const Entity srv_b = s.server_player(b.local_id());
    REQUIRE(!srv_a.is_null());
    const math::Vec3 start_a = s.server_peer.position(srv_a);
    CHECK(horizontal(start_a, s.server_peer.position(srv_b)) > 1.0f); // spawned apart

    // Alice holds D for two seconds: her input drives her player on the server only.
    pa.input.set_key("D", true);
    s.step(120);
    const math::Vec3 moved_a = s.server_peer.position(srv_a);
    CHECK(moved_a.x - start_a.x > 5.0f);
    CHECK(horizontal(s.server_peer.position(srv_b), s.server_peer.position(srv_b)) < 1e-4f);
    // Prediction: alice's own view is ahead of (not behind) the server by about her latency.
    CHECK(pa.position(a_local).x >= moved_a.x - 0.4f);
    CHECK(horizontal(pa.position(a_local), moved_a) < 1.0f);
    // Bob sees alice moving (interpolated ~0.1 s behind plus latency).
    CHECK(pb.position(b_view_of_a).x - start_a.x > 4.0f);
    pa.input.set_key("D", false);
    s.step(60);
    // At rest everything converges.
    const math::Vec3 rest_a = s.server_peer.position(srv_a);
    CHECK(horizontal(pa.position(a_local), rest_a) < 0.1f);
    CHECK(horizontal(pb.position(b_view_of_a), rest_a) < 0.1f);
    CHECK(a.diagnostics().rtt_ms > 40.0f);
    CHECK(a.diagnostics().snapshots_received > 30);
    CHECK(s.server->diagnostics().snapshots_sent > 60);

    // Replicated server objects: crates exist everywhere and proxies are not simulated.
    const Entity crate = pb.find("Crate 0");
    REQUIRE(crate != scene::kNullEntity);
    CHECK(pb.scene.world().get<physics::RigidBodyComponent>(crate)->type == physics::BodyType::Kinematic);
    CHECK(s.server_peer.scene.world().get<physics::RigidBodyComponent>(s.server_peer.find("Crate 0"))->type ==
          physics::BodyType::Dynamic);

    // Network variables: server writes, clients read; clients cannot write.
    REQUIRE(set_var(s.server_peer.scene, srv_a, "score", json::Value(5.0)).ok());
    s.step(20);
    CHECK(get_var(pb.scene, b_view_of_a, "score").as_number() == 5.0);
    CHECK(get_var(pa.scene, a_local, "score").as_number() == 5.0);
    CHECK_FALSE(set_var(pa.scene, a_local, "score", json::Value(99.0)).ok());
    // The relay script counted joins in a variable on a non-networked entity (server only).
    CHECK(get_var(s.server_peer.scene, s.server_peer.find("Relay"), "joined").as_number() == 2.0);

    // Script messages: client -> server script -> that client only.
    REQUIRE(a.send("ping", json::Value(21.0)).ok());
    s.step(30);
    CHECK(pa.find("pong 42") != scene::kNullEntity);
    CHECK(pb.find("pong 42") == scene::kNullEntity);
    // Server broadcast reaches everyone.
    REQUIRE(s.server->send("pong", json::Value(7.0)).ok());
    s.step(30);
    CHECK(pa.find("pong 7") != scene::kNullEntity);
    CHECK(pb.find("pong 7") != scene::kNullEntity);
    CHECK_FALSE(a.send("x", json::Value(std::string(kMaxMessageBytes + 10, 'x'))).ok());

    // Alice leaves: her player is despawned on the server and for bob.
    a.disconnect(0);
    s.step(30);
    CHECK(s.server->clients().size() == 1);
    CHECK_FALSE(s.server_peer.scene.world().alive(srv_a));
    CHECK_FALSE(pb.scene.world().alive(b_view_of_a));
    CHECK(b.clients().size() == 1);
}

TEST_CASE("session: a listen-server host plays alongside remote clients") {
    ServerOptions so;
    so.host_player = true;
    so.host_name = "  Hostess\n";
    Session s(so);
    s.step(1);
    // The host's character exists before anyone joins and is driven by the server's own input.
    const Entity host = s.server->local_player();
    REQUIRE(!host.is_null());
    CHECK(is_local(s.server_peer.scene, host));
    CHECK(has_authority(s.server_peer.scene, host));
    REQUIRE(s.server->clients().size() == 1);
    CHECK(s.server->clients()[0].id == kServerId);
    CHECK(s.server->clients()[0].name == "Hostess");
    CHECK(s.server->diagnostics().clients == 0); // network clients only
    CHECK(s.server_peer.find("server:true:1") != scene::kNullEntity);

    NetClient& a = s.add_client("alice");
    s.step(60);
    REQUIRE(a.connected());
    Peer& pa = *s.peers[0];
    REQUIRE(a.clients().size() == 2);
    Entity a_view_of_host;
    for (const auto& c : a.clients())
        if (c.id == kServerId) {
            CHECK(c.name == "Hostess");
            a_view_of_host = c.player;
        }
    REQUIRE(!a_view_of_host.is_null());
    CHECK_FALSE(is_local(pa.scene, a_view_of_host));
    CHECK(pa.find("client:false:2") != scene::kNullEntity);
    const Entity srv_a = s.server_player(a.local_id());
    REQUIRE(!srv_a.is_null());
    CHECK(horizontal(s.server_peer.position(srv_a), s.server_peer.position(host)) > 1.0f);

    // The host holds D: only the host's character moves, and alice sees it move.
    const math::Vec3 host_start = s.server_peer.position(host);
    const math::Vec3 a_start = s.server_peer.position(srv_a);
    s.server_peer.input.set_key("D", true);
    s.step(120);
    s.server_peer.input.set_key("D", false);
    const math::Vec3 host_moved = s.server_peer.position(host);
    CHECK(host_moved.x - host_start.x > 5.0f);
    CHECK(horizontal(s.server_peer.position(srv_a), a_start) < 1e-3f);
    s.step(30);
    const math::Vec3 host_rest = s.server_peer.position(host);
    CHECK(horizontal(pa.position(a_view_of_host), host_rest) < 0.1f);
    // Alice's input still drives only alice.
    pa.input.set_key("A", true);
    s.step(60);
    pa.input.set_key("A", false);
    CHECK(a_start.x - s.server_peer.position(srv_a).x > 2.0f);
    CHECK(horizontal(s.server_peer.position(host), host_rest) < 1e-3f);
    // The host's character survives clients leaving.
    a.disconnect(0);
    s.step(30);
    CHECK(s.server_peer.scene.world().alive(host));
    CHECK(s.server->clients().size() == 1);
}

TEST_CASE("session: gamepad sticks and buttons reach the server's scripts") {
    Session s;
    LinkConditions lossy;
    lossy.latency_ms = 30;
    lossy.loss = 0.1f;
    lossy.seed = 3;
    NetClient& a = s.add_client("pad", lossy);
    s.step(60);
    REQUIRE(a.connected());
    Peer& pa = *s.peers[0];
    const Entity srv = s.server_player(a.local_id());
    REQUIRE(!srv.is_null());
    const math::Vec3 start = s.server_peer.position(srv);
    // Half tilt to the left for one second: about half speed (4 m/s * 0.5).
    pa.input.set_gamepad_axis(0, gameplay::gamepad_axis_index("LeftX"), -0.6f);
    s.step(60);
    const float moved = start.x - s.server_peer.position(srv).x;
    CAPTURE(moved);
    CHECK(moved == doctest::Approx(2.0f).epsilon(0.2));
    pa.input.set_gamepad_axis(0, gameplay::gamepad_axis_index("LeftX"), 0.0f);
    s.step(30);
    // Three taps of South: each press edge is seen once by the server despite packet loss and
    // the input redundancy that repeats frames.
    for (int i = 0; i < 3; ++i) {
        pa.input.set_gamepad_button(0, gameplay::gamepad_button_index("South"), true);
        s.step(6);
        pa.input.set_gamepad_button(0, gameplay::gamepad_button_index("South"), false);
        s.step(6);
    }
    s.step(30);
    CHECK(get_var(s.server_peer.scene, srv, "jumps").as_number() == 3);
    CHECK(s.server->diagnostics().rejected_messages == 0);
}

TEST_CASE("session: handshake validation and server capacity") {
    ServerOptions so;
    so.max_clients = 1;
    Session s(so);
    NetClient& wrong = s.add_client("eve", {}, "another-game");
    s.step(20);
    CHECK(wrong.failed());
    CHECK(wrong.diagnostics().status.find("rejected") != std::string::npos);
    CHECK(s.server->clients().empty());

    NetClient& first = s.add_client("first");
    s.step(20);
    REQUIRE(first.connected());
    NetClient& second = s.add_client("second");
    s.step(20);
    CHECK(second.failed());
    CHECK(second.diagnostics().status.find("full") != std::string::npos);
    CHECK(s.server->clients().size() == 1);
    CHECK(first.connected());
}

TEST_CASE("session: malformed and abusive client traffic is dropped, then kicked") {
    ServerOptions so;
    so.max_violations = 5;
    Session s(so);
    NetClient& good = s.add_client("good");
    s.step(20);
    REQUIRE(good.connected());

    // A raw endpoint whose first message is garbage, then a truncated hello: refused.
    auto junk = s.network.endpoint();
    const PeerId jp = junk->connect("memory", 7777).value();
    std::vector<TransportEvent> jev;
    junk->poll(0, jev);
    s.step(2);
    const std::uint8_t garbage[5] = {0xFF, 1, 2, 3, 4};
    junk->send(jp, Channel::Reliable, garbage);
    const std::uint8_t short_hello[1] = {1};
    junk->send(jp, Channel::Reliable, short_hello);
    for (int i = 0; i < 5; ++i) junk->poll(0, jev), s.step(1);
    bool junk_dropped = false;
    for (const auto& e : jev) junk_dropped |= e.type == TransportEvent::Type::Disconnected;
    CHECK(junk_dropped);

    // A peer with a valid hello that then floods invalid messages is kicked after 5 violations.
    auto raw = s.network.endpoint();
    const PeerId sp = raw->connect("memory", 7777).value();
    std::vector<TransportEvent> ev;
    raw->poll(0, ev);
    s.step(2);
    ByteWriter hello;
    hello.u8(1);
    hello.u32(0x54434956);
    hello.u32(kProtocolVersion);
    hello.str("arena");
    hello.str("mallory");
    raw->send(sp, Channel::Reliable, hello.data());
    for (int i = 0; i < 5; ++i) raw->poll(0, ev), s.step(1);
    CHECK(s.server->clients().size() == 2);
    for (int i = 0; i < 10; ++i) {
        const std::uint8_t bad[3] = {0xEE, 0, static_cast<std::uint8_t>(i)};
        raw->send(sp, Channel::Reliable, bad);
        const std::uint8_t bad_input[2] = {2, 9}; // truncated input frame
        raw->send(sp, Channel::Unreliable, bad_input);
        raw->poll(0, ev);
        s.step(1);
    }
    for (int i = 0; i < 5; ++i) raw->poll(0, ev), s.step(1);
    bool kicked = false;
    for (const auto& e : ev) kicked |= e.type == TransportEvent::Type::Disconnected;
    CHECK(kicked);
    CHECK(s.server->diagnostics().rejected_messages >= 6);
    CHECK(s.server->clients().size() == 1);
    s.step(30);
    CHECK(good.connected()); // the good client was never affected
    CHECK(good.clients().size() == 1); // mallory's player was removed for everyone
}

TEST_CASE("session: snapshot budget defers low-priority entities but keeps everything fresh") {
    ServerOptions so;
    so.snapshot_budget = 400;
    Session s(so, 150); // 150 spinning markers
    NetClient& c = s.add_client("viewer");
    s.step(90);
    REQUIRE(c.connected());
    const auto d = s.server->diagnostics();
    CHECK(d.snapshot_bytes <= 400 + 64);
    CHECK(d.entities_deferred > 0);
    CHECK(d.entities >= 150);
    // Every marker's rotation reached the client recently despite the budget.
    Peer& p = *s.peers[0];
    int fresh = 0;
    for (int i = 0; i < 150; ++i) {
        const Entity ms = s.server_peer.find("Marker " + std::to_string(i));
        const Entity mc = p.find("Marker " + std::to_string(i));
        REQUIRE(mc != scene::kNullEntity);
        const math::Quat a = s.server_peer.scene.world().get<Transform>(ms)->rotation;
        const math::Quat b = p.scene.world().get<Transform>(mc)->rotation;
        const float dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        // 90 deg/s: within ~1 s of the server's angle.
        if (dot > std::cos(math::radians(90.0f) * 0.5f)) ++fresh;
    }
    CHECK(fresh == 150);
}

TEST_CASE("session: late joiners get the current world and prune destroyed scene objects") {
    Session s;
    s.step(5);
    s.server_peer.scene.destroy(s.server_peer.find("Crate 1"));
    {
        auto* t = s.server_peer.scene.world().get<Transform>(s.server_peer.find("Crate 2"));
        t->position = {2, 0.5f, 2};
    }
    s.step(5);
    NetClient& late = s.add_client("late");
    s.step(40);
    REQUIRE(late.connected());
    Peer& p = *s.peers[0];
    CHECK(p.find("Crate 1") == scene::kNullEntity);
    const Entity c2 = p.find("Crate 2");
    REQUIRE(c2 != scene::kNullEntity);
    CHECK(horizontal(p.position(c2), {2, 0, 2}) < 0.2f);
    CHECK(p.find("Crate 0") != scene::kNullEntity);
}

TEST_CASE("session: delta snapshots omit acknowledged state and stay exact under loss") {
    // Header of a snapshot without entries: type, tick, input ack, echo, held, seq, baseline, count.
    constexpr std::uint32_t kHeader = 1 + 4 + 4 + 8 + 8 + 4 + 4 + 2;
    Session s;
    LinkConditions lossy;
    lossy.latency_ms = 30;
    lossy.jitter_ms = 10;
    lossy.loss = 0.2f;
    lossy.seed = 11;
    NetClient& a = s.add_client("alice", lossy);
    NetClient& b = s.add_client("bob");
    s.step(90);
    REQUIRE(a.connected());
    REQUIRE(b.connected());
    Peer& pa = *s.peers[0];
    Peer& pb = *s.peers[1];
    // Alice walks (her packets and snapshots lose 20%), then everything comes to rest.
    pa.input.set_key("D", true);
    pa.input.set_key("S", true);
    s.step(90);
    pa.input.set_key("D", false);
    pa.input.set_key("S", false);
    s.step(120);
    CHECK(s.server->diagnostics().delta_snapshots > 100);
    CHECK(a.diagnostics().delta_snapshots > 30);
    CHECK(b.diagnostics().delta_snapshots > 30);
    // Every replicated entity decoded to the server's state (millimetre quantization) for both.
    const Entity srv_a = s.server_player(a.local_id());
    Entity b_view_of_a;
    for (const auto& c : b.clients())
        if (c.id == a.local_id()) b_view_of_a = c.player;
    REQUIRE(!b_view_of_a.is_null());
    CHECK(math::length(pb.position(b_view_of_a) - s.server_peer.position(srv_a)) < 2e-3f);
    CHECK(math::length(pa.position(a.local_player()) - s.server_peer.position(srv_a)) < 0.02f);
    for (int i = 0; i < 3; ++i) {
        const std::string name = "Crate " + std::to_string(i);
        const math::Vec3 truth = s.server_peer.position(s.server_peer.find(name));
        CHECK(math::length(pa.position(pa.find(name)) - truth) < 2e-3f);
        CHECK(math::length(pb.position(pb.find(name)) - truth) < 2e-3f);
    }
    // At rest, acknowledged entities are not repeated: snapshots carry only their header.
    s.step(30);
    CHECK(s.server->diagnostics().snapshot_bytes == kHeader);
    CHECK(b.diagnostics().snapshot_bytes == kHeader);
}

TEST_CASE("session: delta snapshots need less bandwidth than full snapshots") {
    // The same scripted minute-fraction (60 markers spinning in place, one player walking) with
    // and without delta encoding; reports the measured server bytes per snapshot.
    auto measure = [](bool delta) {
        ServerOptions so;
        so.delta_snapshots = delta;
        so.snapshot_budget = 4000;
        Session s(so, 60);
        NetClient& c = s.add_client("walker");
        s.step(60);
        REQUIRE(c.connected());
        const auto before = s.server->diagnostics();
        s.peers[0]->input.set_key("D", true);
        s.step(120);
        s.peers[0]->input.set_key("D", false);
        s.step(120);
        const auto after = s.server->diagnostics();
        CHECK(after.entities_deferred == 0);
        return static_cast<double>(after.bytes_sent - before.bytes_sent) /
               static_cast<double>(after.snapshots_sent - before.snapshots_sent);
    };
    const double full = measure(false);
    const double delta = measure(true);
    MESSAGE("bytes per snapshot (incl. reliable traffic): full " << full << ", delta " << delta);
    CHECK(delta < full * 0.6);
}

// ---- secure sessions (Slice 26) -----------------------------------------------------------------

namespace {

// Records what passes through to the wire and can inject packets (an on-path attacker).
class TapTransport final : public Transport {
public:
    explicit TapTransport(std::unique_ptr<Transport> inner) : inner_(std::move(inner)) {}
    Result<PeerId> connect(std::string_view host, std::uint16_t port) override { return inner_->connect(host, port); }
    void send(PeerId peer, Channel channel, std::span<const std::uint8_t> data) override {
        sent.push_back({channel, {data.begin(), data.end()}});
        inner_->send(peer, channel, data);
    }
    void disconnect(PeerId peer) override { inner_->disconnect(peer); }
    void poll(double now, std::vector<TransportEvent>& out) override { inner_->poll(now, out); }
    PeerStats stats(PeerId peer) const override { return inner_->stats(peer); }
    std::string_view name() const override { return inner_->name(); }
    void inject(PeerId peer, Channel channel, const std::vector<std::uint8_t>& data) { inner_->send(peer, channel, data); }
    std::vector<std::pair<Channel, std::vector<std::uint8_t>>> sent;

private:
    std::unique_ptr<Transport> inner_;
};

struct SecurePair {
    MemoryNetwork net;
    std::unique_ptr<Transport> server, client;
    TapTransport* tap{nullptr};
    PeerId to_server{0}, to_client{0};
    std::vector<TransportEvent> se, ce;
    KeyPair server_key = std::move(generate_key_pair()).value();

    SecurePair(SecureOptions co, std::string server_password = {}, bool secure_server = true, bool secure_client = true) {
        std::unique_ptr<Transport> s = std::move(net.listen(9)).value();
        if (secure_server) {
            SecureOptions so;
            so.server_secret = server_key.secret;
            so.password = std::move(server_password);
            s = with_encryption(std::move(s), so);
        }
        server = std::move(s);
        auto t = std::make_unique<TapTransport>(net.endpoint());
        tap = t.get();
        client = secure_client ? with_encryption(std::move(t), std::move(co)) : std::unique_ptr<Transport>(std::move(t));
        to_server = client->connect("memory", 9).value();
    }
    void pump(int rounds = 6, double now = 0) {
        for (int i = 0; i < rounds; ++i) {
            client->poll(now, ce);
            server->poll(now, se);
        }
        for (const auto& e : se)
            if (e.type == TransportEvent::Type::Connected) to_client = e.peer;
    }
    static int count(const std::vector<TransportEvent>& v, TransportEvent::Type t) {
        return static_cast<int>(std::count_if(v.begin(), v.end(), [&](const TransportEvent& e) { return e.type == t; }));
    }
};

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

} // namespace

TEST_CASE("secure: keys, hex and key files") {
    const auto a = generate_key_pair().value();
    const auto b = generate_key_pair().value();
    CHECK(a.secret != b.secret);
    CHECK(public_key_of(a.secret) == a.public_key);
    CHECK(key_from_hex(key_to_hex(a.public_key)).value() == a.public_key);
    CHECK(key_from_hex(std::string(key_to_hex(a.public_key)) + "\n").ok());
    CHECK_FALSE(key_from_hex("abc").ok());
    CHECK_FALSE(key_from_hex(std::string(64, 'g')).ok());
    const auto file = script_dir() / "keys" / "server.key";
    std::filesystem::remove(file);
    const auto created = load_or_create_key(file).value();
    const auto loaded = load_or_create_key(file).value();
    CHECK(created.public_key == loaded.public_key);
    CHECK(created.secret == loaded.secret);
#if !defined(_WIN32)
    const auto perms = std::filesystem::status(file).permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
    std::ofstream(file, std::ios::trunc) << "not a key";
    CHECK_FALSE(load_or_create_key(file).ok());
}

TEST_CASE("secure: handshake, encrypted exchange on both channels and pinned keys") {
    SecureOptions co;
    SecurePair p(co);
    p.pump();
    REQUIRE(SecurePair::count(p.se, TransportEvent::Type::Connected) == 1);
    REQUIRE(SecurePair::count(p.ce, TransportEvent::Type::Connected) == 1);
    CHECK(p.client->encrypted());
    CHECK(p.client->stats(p.to_server).encrypted);
    CHECK(p.client->stats(p.to_server).remote_key == key_to_hex(p.server_key.public_key));
    CHECK(p.client->max_payload() == 64 * 1024 - kSecureOverhead);
    p.se.clear(), p.ce.clear();
    const auto secret = bytes("attack at dawn, the password is swordfish");
    p.client->send(p.to_server, Channel::Reliable, secret);
    p.client->send(p.to_server, Channel::Unreliable, bytes("u1"));
    p.server->send(p.to_client, Channel::Unreliable, bytes("pong"));
    p.server->send(p.to_client, Channel::Reliable, {});
    p.pump();
    REQUIRE(p.se.size() == 2);
    CHECK(p.se[0].data == secret);
    CHECK(p.se[1].channel == Channel::Unreliable);
    CHECK(p.se[1].data == bytes("u1"));
    REQUIRE(p.ce.size() == 2);
    CHECK(p.ce[0].data == bytes("pong"));
    CHECK(p.ce[1].data.empty());
    // Nothing readable went over the wire.
    for (const auto& [ch, pkt] : p.tap->sent)
        CHECK(std::search(pkt.begin(), pkt.end(), secret.begin(), secret.begin() + 13) == pkt.end());

    // Pinning another key: refused before any data flows (the matching case is the next test).
    SecureOptions wrong_pin;
    wrong_pin.expected_server_key = generate_key_pair().value().public_key;
    SecurePair bad(wrong_pin);
    bad.pump();
    CHECK(SecurePair::count(bad.se, TransportEvent::Type::Connected) == 0);
    REQUIRE(SecurePair::count(bad.ce, TransportEvent::Type::Disconnected) == 1);
    CHECK(bad.ce.back().reason.find("server key mismatch") != std::string::npos);
}

TEST_CASE("secure: pinned server key matches across reconnects") {
    const auto kp = generate_key_pair().value();
    MemoryNetwork net;
    SecureOptions so;
    so.server_secret = kp.secret;
    auto server = with_encryption(std::move(net.listen(4)).value(), so);
    for (int i = 0; i < 2; ++i) {
        SecureOptions co;
        co.expected_server_key = kp.public_key;
        auto client = with_encryption(net.endpoint(), co);
        const PeerId sp = client->connect("memory", 4).value();
        std::vector<TransportEvent> se, ce;
        for (int r = 0; r < 6; ++r) client->poll(0, ce), server->poll(0, se);
        REQUIRE(SecurePair::count(ce, TransportEvent::Type::Connected) == 1);
        CHECK(client->stats(sp).remote_key == key_to_hex(kp.public_key));
    }
}

TEST_CASE("secure: passwords, plaintext peers and handshake timeouts") {
    SecureOptions right;
    right.password = "open sesame";
    SecurePair good(right, "open sesame");
    good.pump();
    CHECK(SecurePair::count(good.se, TransportEvent::Type::Connected) == 1);
    CHECK(SecurePair::count(good.ce, TransportEvent::Type::Connected) == 1);
    // The password itself never appears on the wire.
    const auto pw = bytes("open sesame");
    for (const auto& [ch, pkt] : good.tap->sent) CHECK(std::search(pkt.begin(), pkt.end(), pw.begin(), pw.end()) == pkt.end());

    SecureOptions wrong;
    wrong.password = "let me in";
    SecurePair bad(wrong, "open sesame");
    bad.pump(10);
    CHECK(SecurePair::count(bad.se, TransportEvent::Type::Connected) == 0);
    REQUIRE(SecurePair::count(bad.ce, TransportEvent::Type::Disconnected) == 1);
    CHECK(bad.ce.back().reason == "server refused the connection: wrong password");

    SecurePair none({}, "open sesame");
    none.pump(10);
    CHECK(SecurePair::count(none.ce, TransportEvent::Type::Disconnected) == 1);

    // A plaintext client is dropped by a secure server without reaching the session layer.
    SecurePair plain_client({}, {}, true, false);
    plain_client.pump();
    plain_client.client->send(plain_client.to_server, Channel::Reliable, bytes("hello?"));
    plain_client.pump(10);
    CHECK(plain_client.se.empty());
    CHECK(SecurePair::count(plain_client.ce, TransportEvent::Type::Disconnected) == 1);

    // A secure client facing a server that answers with something else gives up with a reason.
    SecurePair plain_server({}, {}, false, true);
    plain_server.pump();
    REQUIRE(plain_server.to_client != 0);
    plain_server.server->send(plain_server.to_client, Channel::Reliable, bytes("welcome, stranger"));
    plain_server.pump();
    REQUIRE(SecurePair::count(plain_server.ce, TransportEvent::Type::Disconnected) == 1);
    CHECK(plain_server.ce.back().reason.find("does not use encryption") != std::string::npos);

    // A server that never answers: the client times out after handshake_timeout.
    SecurePair silent({}, {}, false, true);
    silent.pump(2, 0.0);
    silent.pump(2, 6.0);
    REQUIRE(SecurePair::count(silent.ce, TransportEvent::Type::Disconnected) == 1);
    CHECK(silent.ce.back().reason == "secure handshake timed out");
}

TEST_CASE("secure: forged, corrupted and replayed packets are discarded") {
    SecurePair p({});
    p.pump();
    REQUIRE(p.to_client != 0);
    p.se.clear();
    p.tap->sent.clear();
    p.client->send(p.to_server, Channel::Reliable, bytes("transfer 10 coins"));
    p.client->send(p.to_server, Channel::Unreliable, bytes("move left"));
    p.pump();
    REQUIRE(p.se.size() == 2);
    REQUIRE(p.tap->sent.size() == 2);
    const auto reliable = p.tap->sent[0].second;
    const auto unreliable = p.tap->sent[1].second;
    p.se.clear();
    // Replays on either channel.
    p.tap->inject(p.to_server, Channel::Reliable, reliable);
    p.tap->inject(p.to_server, Channel::Unreliable, unreliable);
    // A flipped ciphertext bit, a flipped counter, a packet moved to the other channel, junk.
    auto flipped = reliable;
    flipped[12] ^= 0x01;
    auto recount = unreliable;
    recount[1] ^= 0x40;
    p.tap->inject(p.to_server, Channel::Reliable, flipped);
    p.tap->inject(p.to_server, Channel::Unreliable, recount);
    p.tap->inject(p.to_server, Channel::Unreliable, reliable);
    p.tap->inject(p.to_server, Channel::Unreliable, std::vector<std::uint8_t>(40, 0xA3));
    p.tap->inject(p.to_server, Channel::Unreliable, std::vector<std::uint8_t>(3, 0xA3));
    p.pump();
    CHECK(p.se.empty());
    CHECK(p.server->stats(p.to_client).packets_rejected == 7);
    // Out-of-order unreliable packets inside the window are still accepted once each.
    p.tap->sent.clear();
    for (int i = 0; i < 3; ++i) p.client->send(p.to_server, Channel::Unreliable, bytes("u" + std::to_string(i)));
    REQUIRE(p.tap->sent.size() == 3);
    auto late = p.tap->sent;
    p.tap->sent.clear();
    p.pump(); // delivered in order: u0 u1 u2
    p.se.clear();
    for (int i = 0; i < 40; ++i) p.client->send(p.to_server, Channel::Unreliable, bytes("n"));
    p.pump();
    CHECK(p.se.size() == 40);
    p.se.clear();
    p.tap->inject(p.to_server, Channel::Unreliable, late[1].second); // 41 packets old, already seen
    p.pump();
    CHECK(p.se.empty());
    // The session still works afterwards.
    p.client->send(p.to_server, Channel::Reliable, bytes("still here"));
    p.pump();
    REQUIRE(p.se.size() == 1);
    CHECK(p.se[0].data == bytes("still here"));
}

TEST_CASE("secure: a full game session runs encrypted over a lossy link") {
    const auto kp = generate_key_pair().value();
    MemoryNetwork network;
    Peer server_peer;
    SecureOptions so;
    so.server_secret = kp.secret;
    so.password = "arena-pass";
    ServerOptions sopt;
    sopt.game_id = "arena";
    NetServer server(with_encryption(std::move(network.listen(7778)).value(), so), sopt);
    server.install(server_peer.sim);
    LinkConditions link;
    link.latency_ms = 40;
    link.jitter_ms = 10;
    link.loss = 0.05f;
    link.seed = 7;
    std::vector<std::unique_ptr<Peer>> peers;
    std::vector<std::unique_ptr<NetClient>> clients;
    for (const char* name : {"ann", "bob"}) {
        peers.push_back(std::make_unique<Peer>());
        SecureOptions co;
        co.expected_server_key = kp.public_key;
        co.password = "arena-pass";
        clients.push_back(std::make_unique<NetClient>(with_encryption(with_conditions(network.endpoint(), link), co),
                                                      client_options(7778, name)));
        clients.back()->install(peers.back()->sim);
    }
    // An intruder without the password never becomes a player.
    Peer intruder_peer;
    SecureOptions ico;
    ico.password = "guess";
    NetClient intruder(with_encryption(network.endpoint(), ico), client_options(7778, "eve"));
    intruder.install(intruder_peer.sim);
    for (int i = 0; i < 240; ++i) {
        peers[0]->input.set_key("D", true);
        server_peer.tick();
        for (auto& p : peers) p->tick();
        intruder_peer.tick();
    }
    REQUIRE(clients[0]->connected());
    REQUIRE(clients[1]->connected());
    CHECK(server.clients().size() == 2);
    CHECK(server.diagnostics().encrypted);
    const auto d = clients[0]->diagnostics();
    CHECK(d.encrypted);
    CHECK(d.server_key == key_to_hex(kp.public_key));
    CHECK(d.snapshots_received > 100);
    CHECK(intruder.failed());
    CHECK(intruder.diagnostics().status == "server refused the connection: wrong password");
    // Ann walked right on the server and Bob sees it.
    const Entity ann = [&] {
        for (const auto& c : server.clients())
            if (c.name == "ann") return c.player;
        return Entity{};
    }();
    REQUIRE_FALSE(ann.is_null());
    CHECK(server_peer.position(ann).x > 5.0f);
}

TEST_CASE("secure: encrypted UDP loopback over ENet") {
    const auto kp = generate_key_pair().value();
    UdpOptions so_udp;
    so_udp.port = 27991;
    so_udp.bind_address = "127.0.0.1";
    auto st = create_udp_transport(so_udp);
    REQUIRE(st);
    SecureOptions so;
    so.server_secret = kp.secret;
    auto server = with_encryption(std::move(*st), so);
    SecureOptions co;
    co.expected_server_key = kp.public_key;
    auto client = with_encryption(std::move(create_udp_transport({})).value(), co);
    const PeerId sp = client->connect("127.0.0.1", 27991).value();
    std::vector<TransportEvent> se, ce;
    PeerId cp = 0;
    const auto start = std::chrono::steady_clock::now();
    bool got = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5) && !got) {
        client->poll(0, ce);
        server->poll(0, se);
        for (auto& e : se) {
            if (e.type == TransportEvent::Type::Connected) {
                cp = e.peer;
                server->send(cp, Channel::Reliable, bytes("hello over udp"));
            }
        }
        se.clear();
        for (auto& e : ce)
            if (e.type == TransportEvent::Type::Received && e.data == bytes("hello over udp")) got = true;
        ce.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(got);
    CHECK(client->stats(sp).encrypted);
    CHECK(client->stats(sp).remote_key == key_to_hex(kp.public_key));
}
