// SPDX-License-Identifier: Apache-2.0
// Networking: transports, a server-authoritative client/server session and entity replication.
//
// Layers (docs/NETWORKING_SPEC.md):
//   Transport        - connection-oriented datagrams with a reliable-ordered and an unreliable
//                      channel. Implementations: UDP (ENet), an in-memory network for tests and
//                      tools, and a decorator that emulates latency/jitter/loss/bandwidth.
//   wire.hpp         - bounds-checked binary serialization of untrusted payloads.
//   NetServer        - authoritative simulation: accepts clients, spawns a player per client from
//                      the scene's player template, applies each client's input to the entities
//                      it owns, replicates spawns/despawns, transforms (snapshots within a
//                      bandwidth budget with relevancy and priority) and script variables.
//   NetClient        - connects, mirrors the server's entities, predicts its own player with local
//                      input, reconciles against server snapshots and interpolates the others.
// Both install systems into a gameplay::Simulation and publish a NetContext on the scene that
// scripts use (`net.*` in Luau). Without a session a scene runs offline: player templates are
// ordinary entities and every script has authority.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::net {

using scene::Entity;

inline constexpr std::uint32_t kProtocolVersion = 2; // 2: gamepad state in input frames
inline constexpr std::uint16_t kDefaultPort = 27960;
inline constexpr std::uint16_t kServerId = 0;      // sender id of messages from the server
inline constexpr std::uint16_t kAllClients = 0xFFFF;

// ---- components -------------------------------------------------------------------------------

// Marks an entity as networked. Scene entities with this component are mirrored on clients and
// driven by the server; a `player_template` entity is instead instantiated once per connected
// client (that client owns the copy: its input drives the copy's scripts on the server and it
// predicts the copy locally). Offline, a template is just a normal entity (single-player).
struct NetworkIdentityComponent {
    bool player_template{false};
    bool replicate_transform{true};
    float relevancy_radius{0.0f}; // transforms are sent to clients whose player is within this
                                  // distance; 0 = always relevant
    float priority{1.0f};         // relative share of the snapshot budget
    friend bool operator==(const NetworkIdentityComponent&, const NetworkIdentityComponent&) = default;
};

// Runtime state of a networked entity (not serialized). Present on entities a session tracks
// and on any entity a script stored a network variable on.
struct NetState {
    std::uint32_t net_id{0};      // 0 = not (yet) replicated
    std::uint16_t owner{kServerId};
    bool local{false};            // client: this machine owns and predicts it
    bool proxy{false};            // client: driven by snapshots, simulation disabled
    json::Value vars = json::Value::object(); // replicated script variables (server -> clients)
};

void register_components(); // "NetworkIdentity"

// ---- transport --------------------------------------------------------------------------------

enum class Channel : std::uint8_t { Reliable = 0, Unreliable = 1 };
using PeerId = std::uint32_t; // 0 = invalid

struct TransportEvent {
    enum class Type : std::uint8_t { Connected, Disconnected, Received };
    Type type{Type::Received};
    PeerId peer{0};
    Channel channel{Channel::Reliable};
    std::vector<std::uint8_t> data;
};

struct PeerStats {
    float rtt_ms{0};               // transport round-trip estimate (0 when unknown)
    std::uint64_t bytes_sent{0}, bytes_received{0};
    std::uint64_t packets_sent{0}, packets_received{0};
    std::uint64_t packets_dropped{0}; // emulated drops (LinkConditions)
};

class Transport {
public:
    virtual ~Transport() = default;
    // Starts connecting to a server; a Connected (or Disconnected on failure) event follows.
    [[nodiscard]] virtual Result<PeerId> connect(std::string_view host, std::uint16_t port) = 0;
    virtual void send(PeerId peer, Channel channel, std::span<const std::uint8_t> data) = 0;
    virtual void disconnect(PeerId peer) = 0;
    // Sends queued data and appends received events. `now` is the caller's clock in seconds
    // (used by emulation; real transports keep their own timers).
    virtual void poll(double now, std::vector<TransportEvent>& out) = 0;
    [[nodiscard]] virtual PeerStats stats(PeerId peer) const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
    // Largest payload accepted per send (larger sends are dropped and logged).
    [[nodiscard]] virtual std::size_t max_payload() const { return 64 * 1024; }
};

struct UdpOptions {
    std::uint16_t port{0};          // listen port (server); 0 = client (ephemeral port)
    std::string bind_address;       // empty = any
    std::uint32_t max_peers{32};
    double connect_timeout{5.0};    // seconds
};
// UDP transport over ENet 1.3 (reliable/unreliable channels, fragmentation, RTT, timeouts).
[[nodiscard]] Result<std::unique_ptr<Transport>> create_udp_transport(const UdpOptions& options);

// Deterministic in-memory network: endpoints connect by port number and exchange packets on
// poll(). Reliable data is delivered in order; nothing is lost unless wrapped with conditions.
class MemoryNetwork {
public:
    MemoryNetwork();
    ~MemoryNetwork();
    // A server endpoint listening on `port` (fails if taken).
    [[nodiscard]] Result<std::unique_ptr<Transport>> listen(std::uint16_t port);
    // A client endpoint; call connect("memory", port).
    [[nodiscard]] std::unique_ptr<Transport> endpoint();
    struct State;

private:
    std::shared_ptr<State> state_;
};

// Network emulation applied to everything a transport sends. Unreliable packets can be lost and
// reordered by jitter; reliable packets are never lost (a "lost" one is delayed by a retransmit
// round trip, as a reliable protocol would) and stay in order. Wrap both ends for symmetric links.
struct LinkConditions {
    float latency_ms{0};      // one-way base delay
    float jitter_ms{0};       // uniform extra delay 0..jitter
    float loss{0};            // 0..1 probability per packet
    float bandwidth_kbps{0};  // outgoing cap in kilobits per second; 0 = unlimited
    std::uint64_t seed{1};
    [[nodiscard]] bool active() const noexcept { return latency_ms > 0 || jitter_ms > 0 || loss > 0 || bandwidth_kbps > 0; }
};
[[nodiscard]] std::unique_ptr<Transport> with_conditions(std::unique_ptr<Transport> inner, const LinkConditions& conditions);
// Parses "latency=80,jitter=10,loss=0.05,bandwidth=256" (any subset).
[[nodiscard]] Result<LinkConditions> parse_conditions(std::string_view text);

// ---- session context (scripts, gameplay) ------------------------------------------------------

enum class Role : std::uint8_t { Offline, Server, Client };
[[nodiscard]] const char* to_string(Role role) noexcept;

struct NetMessage {
    std::uint16_t sender{kServerId};
    std::string name;
    json::Value value;
};

struct ClientInfo {
    std::uint16_t id{0};
    std::string name;
    Entity player;
    float rtt_ms{0};
};

struct NetDiagnostics {
    Role role{Role::Offline};
    bool connected{false};
    std::uint32_t tick{0};             // server tick (client: latest received)
    std::uint32_t clients{0};
    std::uint32_t entities{0};         // replicated entities
    float rtt_ms{0};                   // client: measured round trip to the server
    float send_kbps{0}, receive_kbps{0}; // rolling one-second averages
    std::uint64_t bytes_sent{0}, bytes_received{0};
    std::uint64_t snapshots_sent{0}, snapshots_received{0};
    std::uint32_t snapshot_bytes{0};   // size of the last snapshot sent/received
    std::uint32_t entities_deferred{0}; // server: entities left out of the last snapshot by the budget
    float prediction_error{0};         // client: last reconciliation error of the local player (m)
    std::uint64_t corrections{0};      // client: reconciliations that moved the local player
    std::uint64_t rejected_messages{0}; // server: invalid/over-limit client payloads dropped
    std::string status;                // human-readable state ("connected", "rejected: ...")
};

// What scripts and gameplay code see of the session (published on the scene by both sessions).
class NetContext {
public:
    virtual ~NetContext() = default;
    [[nodiscard]] virtual Role role() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t local_id() const noexcept = 0; // server: 0
    [[nodiscard]] virtual bool connected() const noexcept = 0;
    // Client: to the server (`to` ignored). Server: to one client or kAllClients. Reliable,
    // ordered. Value size is limited to kMaxMessageBytes once encoded.
    virtual Status send(std::string_view name, const json::Value& value, std::uint16_t to = kAllClients) = 0;
    // Messages received since the last call (script host dispatches them to on_net_message).
    [[nodiscard]] virtual std::vector<NetMessage> take_messages() = 0;
    [[nodiscard]] virtual std::vector<ClientInfo> clients() const = 0;
    [[nodiscard]] virtual Entity local_player() const noexcept = 0; // client: the predicted player
    [[nodiscard]] virtual NetDiagnostics diagnostics() const = 0;
};

inline constexpr std::size_t kMaxMessageBytes = 1024;
inline constexpr std::size_t kMaxVarBytes = 512;
inline constexpr std::size_t kMaxNameBytes = 64;

[[nodiscard]] NetContext* context_of(scene::Scene& scene);
void bind(scene::Scene& scene, NetContext* context);
[[nodiscard]] Role role_of(scene::Scene& scene);
// True when this process may change shared state of `e`: offline, or on the server.
[[nodiscard]] bool has_authority(scene::Scene& scene, Entity e);
// Owner client of `e` or its nearest networked ancestor (kServerId if none).
[[nodiscard]] std::uint16_t owner_of(scene::Scene& scene, Entity e);
// True when this machine's player input drives `e`: always offline; on the server for entities it
// owns; on a client for its own (predicted) player hierarchy.
[[nodiscard]] bool is_local(scene::Scene& scene, Entity e);
// Network variables: stored on NetState (added on demand), replicated server -> clients.
// Fails without authority or when the value is too large / not JSON-representable.
Status set_var(scene::Scene& scene, Entity e, std::string_view key, const json::Value& value);
[[nodiscard]] json::Value get_var(scene::Scene& scene, Entity e, std::string_view key);

// ---- sessions ---------------------------------------------------------------------------------

struct ServerOptions {
    std::string game_id;              // clients must present the same id (project + scene)
    std::uint16_t max_clients{8};
    float snapshot_rate{30.0f};       // snapshots per second per client
    std::uint32_t snapshot_budget{1200}; // bytes per snapshot packet (excess entities deferred)
    std::uint32_t input_buffer{4};    // max queued input frames per client before catch-up
    float message_rate{60.0f};        // reliable client messages per second allowed (burst = rate)
    std::uint32_t max_violations{20}; // malformed payloads before a client is kicked
    float spawn_spacing{2.5f};        // player copies are placed on a ring around the template
};

class NetServer final : public NetContext {
public:
    NetServer(std::unique_ptr<Transport> transport, ServerOptions options = {});
    ~NetServer() override;
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Installs "net.receive" (PreUpdate: connections, client input) and "net.send" (PostUpdate:
    // replication). Clears sim.input routing to per-client input for owned entities.
    void install(gameplay::Simulation& sim);
    // Disconnects every client (they see "server shutting down") and flushes.
    void shutdown(double now);

    [[nodiscard]] Role role() const noexcept override { return Role::Server; }
    [[nodiscard]] std::uint16_t local_id() const noexcept override { return kServerId; }
    [[nodiscard]] bool connected() const noexcept override { return true; }
    Status send(std::string_view name, const json::Value& value, std::uint16_t to = kAllClients) override;
    [[nodiscard]] std::vector<NetMessage> take_messages() override;
    [[nodiscard]] std::vector<ClientInfo> clients() const override;
    [[nodiscard]] Entity local_player() const noexcept override { return {}; }
    [[nodiscard]] NetDiagnostics diagnostics() const override;

    // Manual driving (tests, tools); install() calls these from its systems.
    void receive(scene::Scene& scene, double now);
    void replicate(scene::Scene& scene, double now, float dt);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

struct ClientOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{kDefaultPort};
    std::string name{"player"};
    std::string game_id;
    float interpolation_delay{0.1f};  // seconds behind the newest snapshot for remote entities
    float correction_rate{12.0f};     // how fast prediction errors are blended away (1/s)
    float snap_distance{2.0f};        // errors beyond this teleport the local player
    float input_redundancy{4};        // input frames repeated in each packet (loss tolerance)
    double timeout{10.0};             // seconds without server traffic before giving up
};

class NetClient final : public NetContext {
public:
    NetClient(std::unique_ptr<Transport> transport, ClientOptions options = {});
    ~NetClient() override;
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Installs "net.receive" (PreUpdate) and "net.send" (PostUpdate). The simulation's input is
    // sampled every tick and sent to the server.
    void install(gameplay::Simulation& sim);
    void disconnect(double now);

    [[nodiscard]] Role role() const noexcept override { return Role::Client; }
    [[nodiscard]] std::uint16_t local_id() const noexcept override;
    [[nodiscard]] bool connected() const noexcept override;
    Status send(std::string_view name, const json::Value& value, std::uint16_t to = kAllClients) override;
    [[nodiscard]] std::vector<NetMessage> take_messages() override;
    [[nodiscard]] std::vector<ClientInfo> clients() const override;
    [[nodiscard]] Entity local_player() const noexcept override;
    [[nodiscard]] NetDiagnostics diagnostics() const override;

    // True once the server refused or dropped the connection (see diagnostics().status).
    [[nodiscard]] bool failed() const noexcept;

    void receive(scene::Scene& scene, double now, const gameplay::InputSource* input);
    void replicate(scene::Scene& scene, double now, float dt);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Identifies a game build/content for the handshake: project name + scene path.
[[nodiscard]] std::string make_game_id(std::string_view project_name, std::string_view scene_path);
// Splits "host:port" (port optional, default kDefaultPort).
[[nodiscard]] Result<std::pair<std::string, std::uint16_t>> parse_address(std::string_view text);

} // namespace victus::net
