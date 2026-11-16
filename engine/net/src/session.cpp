// SPDX-License-Identifier: Apache-2.0
// Client/server sessions: handshake, input, spawn/despawn, snapshots, variables, messages.
#include "victus/net/net.hpp"

#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/net/wire.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace victus::net {

using scene::Scene;
using scene::Transform;

namespace {

// ---- protocol ---------------------------------------------------------------------------------

constexpr std::uint32_t kMagic = 0x54434956; // "VICT"
enum Msg : std::uint8_t {
    // client -> server
    kHello = 1,
    kInput = 2,
    kClientMessage = 3,
    kClientGoodbye = 4,
    // server -> client
    kWelcome = 10,
    kReject = 11,
    kSpawn = 12,
    kDespawn = 13,
    kVars = 14,
    kServerMessage = 15,
    kSnapshot = 16,
    kServerGoodbye = 17,
    kRoster = 18,
};
enum SpawnKind : std::uint8_t { kSceneEntity = 0, kPlayer = 1 };
constexpr std::size_t kMaxInputFrames = 8;
constexpr std::size_t kMaxGameId = 256;
constexpr std::size_t kMaxPlayerName = 32;
constexpr float kResendInterval = 1.0f; // unchanged transforms are re-sent this often (loss recovery)

// Key names shared with platform::key_name (the net module has no platform dependency).
constexpr const char* kKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Escape", "Enter", "Tab", "Backspace", "Space", "Delete",
    "Insert", "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down", "LeftShift", "RightShift", "LeftCtrl",
    "RightCtrl", "LeftAlt", "RightAlt", "LeftSuper", "RightSuper", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9",
    "F10", "F11", "F12", "Minus", "Equals", "LeftBracket", "RightBracket", "Backslash", "Semicolon", "Apostrophe", "Grave",
    "Comma", "Period", "Slash"};
constexpr std::size_t kKeyCount = std::size(kKeys);
static_assert(kKeyCount <= 128);

int key_index(std::string_view name) {
    for (std::size_t i = 0; i < kKeyCount; ++i)
        if (name == kKeys[i]) return static_cast<int>(i);
    return -1;
}

struct KeyBits {
    std::uint64_t lo{0}, hi{0};
    [[nodiscard]] bool test(int i) const noexcept { return i >= 0 && (i < 64 ? (lo >> i) & 1 : (hi >> (i - 64)) & 1); }
    void set(int i) noexcept {
        if (i < 0) return;
        if (i < 64) lo |= 1ull << i;
        else hi |= 1ull << (i - 64);
    }
    KeyBits operator|(KeyBits o) const noexcept { return {lo | o.lo, hi | o.hi}; }
};

struct InputFrame {
    std::uint32_t seq{0};
    KeyBits keys;
    std::uint8_t mouse{0};
    // Gamepads merged into one: any pad's buttons, the strongest deflection per axis.
    bool pad{false};
    std::uint16_t pad_buttons{0};
    std::array<std::int16_t, gameplay::kGamepadAxisCount> pad_axes{};
};

std::int16_t quantize_axis(float v) {
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}
float axis_value(std::int16_t q) { return static_cast<float>(q) / 32767.0f; }

InputFrame sample(const gameplay::InputSource* in, std::uint32_t seq) {
    InputFrame f;
    f.seq = seq;
    if (!in) return f;
    for (std::size_t i = 0; i < kKeyCount; ++i)
        if (in->key_down(kKeys[i])) f.keys.set(static_cast<int>(i));
    for (int b = 0; b < 8; ++b)
        if (in->mouse_down(b)) f.mouse |= static_cast<std::uint8_t>(1u << b);
    std::array<float, gameplay::kGamepadAxisCount> axes{};
    for (int p = 0; p < gameplay::kMaxGamepads; ++p) {
        if (!in->gamepad_connected(p)) continue;
        f.pad = true;
        for (int b = 0; b < gameplay::kGamepadButtonCount; ++b)
            if (in->gamepad_button(p, b)) f.pad_buttons |= static_cast<std::uint16_t>(1u << b);
        for (int a = 0; a < gameplay::kGamepadAxisCount; ++a) {
            const float v = in->gamepad_axis(p, a);
            if (std::fabs(v) > std::fabs(axes[static_cast<std::size_t>(a)])) axes[static_cast<std::size_t>(a)] = v;
        }
    }
    for (std::size_t a = 0; a < axes.size(); ++a) f.pad_axes[a] = quantize_axis(axes[a]);
    return f;
}

// Input of one remote client as seen by the server's scripts.
class RemoteInput final : public gameplay::InputSource {
public:
    void apply(const InputFrame& f) {
        hold();
        cur_ = f.keys;
        mouse_ = f.mouse;
        pad_ = f.pad;
        buttons_ = f.pad_buttons;
        axes_ = f.pad_axes;
    }
    void hold() {
        prev_ = cur_;
        prev_mouse_ = mouse_;
        prev_buttons_ = buttons_;
        prev_axes_ = axes_;
    }
    [[nodiscard]] bool key_down(std::string_view k) const override { return cur_.test(key_index(k)); }
    [[nodiscard]] bool key_pressed(std::string_view k) const override {
        const int i = key_index(k);
        return cur_.test(i) && !prev_.test(i);
    }
    [[nodiscard]] bool key_released(std::string_view k) const override {
        const int i = key_index(k);
        return !cur_.test(i) && prev_.test(i);
    }
    [[nodiscard]] bool mouse_down(int b) const override { return b >= 0 && b < 8 && ((mouse_ >> b) & 1); }
    [[nodiscard]] bool mouse_pressed(int b) const override { return mouse_down(b) && !((prev_mouse_ >> b) & 1); }
    [[nodiscard]] math::Vec2 mouse_position() const override { return {}; }
    [[nodiscard]] math::Vec2 mouse_delta() const override { return {}; }
    [[nodiscard]] bool mouse_released(int b) const override {
        return b >= 0 && b < 8 && !((mouse_ >> b) & 1) && ((prev_mouse_ >> b) & 1);
    }
    [[nodiscard]] bool gamepad_connected(int pad) const override { return pad == 0 && pad_; }
    [[nodiscard]] bool gamepad_button(int pad, int b, bool previous = false) const override {
        if (pad != 0 || b < 0 || b >= gameplay::kGamepadButtonCount) return false;
        return (((previous ? prev_buttons_ : buttons_) >> b) & 1u) != 0;
    }
    [[nodiscard]] float gamepad_axis(int pad, int a, bool previous = false) const override {
        if (pad != 0 || a < 0 || a >= gameplay::kGamepadAxisCount) return 0;
        return axis_value((previous ? prev_axes_ : axes_)[static_cast<std::size_t>(a)]);
    }

private:
    KeyBits cur_, prev_;
    std::uint8_t mouse_{0}, prev_mouse_{0};
    bool pad_{false};
    std::uint16_t buttons_{0}, prev_buttons_{0};
    std::array<std::int16_t, gameplay::kGamepadAxisCount> axes_{}, prev_axes_{};
};

std::vector<std::uint8_t> encode_json(const json::Value& v) {
    const std::string s = json::stringify(v, false);
    return {s.begin(), s.end()};
}

Result<json::Value> decode_json(const std::string& text, std::size_t max_bytes) {
    json::ParseLimits limits;
    limits.max_depth = 16;
    limits.max_bytes = max_bytes;
    limits.max_string_bytes = max_bytes;
    return json::parse(text, limits);
}

bool valid_name(std::string_view n) {
    if (n.empty() || n.size() > kMaxNameBytes) return false;
    return std::all_of(n.begin(), n.end(), [](char c) { return c > 31 && c < 127; });
}

std::string clean_player_name(std::string n) {
    std::string out;
    for (const char c : n)
        if (c > 31 && c < 127 && out.size() < kMaxPlayerName) out.push_back(c);
    return out.empty() ? std::string("player") : out;
}

void write_uuid(ByteWriter& w, Uuid u) {
    w.u64(u.hi);
    w.u64(u.lo);
}
Uuid read_uuid(ByteReader& r) {
    Uuid u;
    u.hi = r.u64();
    u.lo = r.u64();
    return u;
}

// ---- snapshot states ---------------------------------------------------------------------------
// Snapshots carry quantized transforms so server and client agree bit-for-bit on what a client
// knows: positions in millimetres, rotations as the wire's 16-bit components. Each snapshot is
// encoded against a baseline (an earlier snapshot the client acknowledged); entities equal to
// their baseline are omitted and the rest send small zigzag-varint deltas per component.

constexpr float kPosScale = 1000.0f; // 1 mm
constexpr std::size_t kServerViews = 32; // sent snapshots remembered per client (~1 s at 30 Hz)
constexpr std::size_t kClientViews = 64;

struct QState {
    std::array<std::int32_t, 3> p{};
    std::array<std::uint16_t, 4> q{32768, 32768, 32768, 65535}; // identity
    bool operator==(const QState&) const = default;
};

QState quantize(math::Vec3 p, math::Quat q) {
    QState s;
    const float in[3] = {p.x, p.y, p.z};
    for (int i = 0; i < 3; ++i)
        s.p[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(std::clamp(std::llround(static_cast<double>(in[i]) * kPosScale), -1'000'000'000ll, 1'000'000'000ll));
    if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
    const float c[4] = {q.x, q.y, q.z, q.w};
    for (int i = 0; i < 4; ++i) {
        const float k = std::clamp(c[i], -1.0f, 1.0f);
        s.q[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(static_cast<std::int32_t>(k * 32767.0f) + 32768);
    }
    return s;
}
math::Vec3 position_of(const QState& s) {
    return {static_cast<float>(s.p[0]) / kPosScale, static_cast<float>(s.p[1]) / kPosScale, static_cast<float>(s.p[2]) / kPosScale};
}
math::Quat rotation_of(const QState& s) {
    float c[4];
    for (int i = 0; i < 4; ++i) c[i] = (static_cast<float>(s.q[static_cast<std::size_t>(i)]) - 32768.0f) / 32767.0f;
    const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
    if (len < 1e-3f) return {};
    return {c[0] / len, c[1] / len, c[2] / len, c[3] / len};
}

// What a client knows after decoding one snapshot: net id -> state, sorted by id.
using View = std::vector<std::pair<std::uint32_t, QState>>;
const QState* find_state(const View& v, std::uint32_t id) {
    const auto it = std::lower_bound(v.begin(), v.end(), id, [](const auto& e, std::uint32_t k) { return e.first < k; });
    return it != v.end() && it->first == id ? &it->second : nullptr;
}
void put_state(View& v, std::uint32_t id, const QState& st) {
    const auto it = std::lower_bound(v.begin(), v.end(), id, [](const auto& e, std::uint32_t k) { return e.first < k; });
    if (it != v.end() && it->first == id) it->second = st;
    else v.insert(it, {id, st});
}
std::uint64_t zigzag(std::int64_t v) { return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63); }
std::int64_t unzigzag(std::uint64_t v) { return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1); }

enum EntryFlags : std::uint32_t { kHasPosition = 1, kHasRotation = 2 };

void write_entry(ByteWriter& w, std::uint32_t id, const QState& st, const QState& base) {
    std::uint32_t flags = 0;
    if (st.p != base.p) flags |= kHasPosition;
    if (st.q != base.q) flags |= kHasRotation;
    w.varuint((static_cast<std::uint64_t>(id) << 2) | flags);
    if (flags & kHasPosition)
        for (std::size_t i = 0; i < 3; ++i) w.varuint(zigzag(static_cast<std::int64_t>(st.p[i]) - base.p[i]));
    if (flags & kHasRotation)
        for (std::size_t i = 0; i < 4; ++i) w.varuint(zigzag(static_cast<std::int64_t>(st.q[i]) - base.q[i]));
}

// ---- scene helpers ----------------------------------------------------------------------------

struct Template {
    Uuid uuid;
    json::Value snapshot; // save_subtree() of the template entity
    math::Vec3 position;
    math::Quat rotation;
};

// Removes the scene's player templates, keeping their snapshots for instantiation.
std::vector<Template> park_templates(Scene& scene) {
    std::vector<Template> out;
    std::vector<Entity> found;
    scene.world().each<NetworkIdentityComponent>([&](Entity e, NetworkIdentityComponent& id) {
        if (id.player_template) found.push_back(e);
    });
    for (const Entity e : found) {
        if (!scene.world().alive(e)) continue;
        Template t;
        t.uuid = scene.uuid(e);
        t.snapshot = scene.save_subtree(e);
        if (const auto* tr = scene.world().get<Transform>(e)) {
            t.position = tr->position;
            t.rotation = tr->rotation;
        }
        scene.destroy(e);
        out.push_back(std::move(t));
    }
    return out;
}

// Instantiates a parked template with fresh UUIDs. The copy is no longer a template.
Result<Entity> instantiate(Scene& scene, const Template& t, std::string_view name) {
    json::Value snaps = t.snapshot;
    std::unordered_map<std::string, std::string> remap;
    for (const auto& ev : snaps.as_array()) remap[ev["uuid"].as_string_or("")] = Uuid::generate().to_string();
    bool first = true;
    for (auto& ev : snaps.as_array()) {
        json::Value out = json::Value::object();
        for (const auto& [k, v] : ev.as_object()) {
            if (k == "uuid") out.set("uuid", remap[v.as_string_or("")]);
            else if (k == "parent" || k == "next") {
                const auto it = remap.find(v.as_string_or(""));
                if (it != remap.end()) out.set(k, it->second);
                else if (k == "parent") out.set(k, v); // the template's own parent
            } else if (k == "name" && first) out.set("name", std::string(name));
            else out.set(k, v);
        }
        ev = std::move(out);
        first = false;
    }
    VICTUS_TRY_STATUS(scene.restore_subtree(snaps));
    const Entity root = scene.find([&] {
        Uuid u;
        (void)Uuid::parse(snaps[0]["uuid"].as_string_or(""), u);
        return u;
    }());
    if (root.is_null()) return make_error(ErrorCode::Unknown, "player template instantiation failed");
    if (auto* id = scene.world().get<NetworkIdentityComponent>(root)) id->player_template = false;
    return root;
}

NetState& state_of(Scene& scene, Entity e) {
    if (auto* s = scene.world().get<NetState>(e)) return *s;
    return scene.world().add<NetState>(e);
}

// Clients do not simulate entities they do not own: dynamic bodies follow snapshots kinematically
// and characters stop falling between snapshot positions.
void make_proxy(Scene& scene, Entity e) {
    auto& w = scene.world();
    if (auto* rb = w.get<physics::RigidBodyComponent>(e); rb && rb->type == physics::BodyType::Dynamic) rb->type = physics::BodyType::Kinematic;
    if (auto* rb = w.get<physics::RigidBody2DComponent>(e); rb && rb->type == physics::BodyType::Dynamic) rb->type = physics::BodyType::Kinematic;
    if (auto* cc = w.get<physics::CharacterControllerComponent>(e)) cc->gravity_scale = 0;
    if (auto* cc = w.get<physics::CharacterController2DComponent>(e)) cc->gravity_scale = 0;
}

struct Binding {
    NetContext* context{nullptr};
};

// Rolling byte counters for kbps diagnostics.
struct Meter {
    double window_start{0};
    std::uint64_t window_bytes{0};
    float kbps{0};
    std::uint64_t total{0};
    void add(std::size_t n) {
        window_bytes += n;
        total += n;
    }
    void tick(double now) {
        if (now - window_start >= 1.0) {
            kbps = static_cast<float>(static_cast<double>(window_bytes) * 8.0 / 1000.0 / std::max(1e-6, now - window_start));
            window_bytes = 0;
            window_start = now;
        }
    }
};

} // namespace

// ---- components / context ---------------------------------------------------------------------

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<NetworkIdentityComponent>("NetworkIdentity", "Networking")
            .field(&NetworkIdentityComponent::player_template, "player_template",
                   {.tooltip = "Spawned once per connected client (who owns and controls the copy); offline it is the local player"})
            .field(&NetworkIdentityComponent::replicate_transform, "replicate_transform", {.tooltip = "Send position/rotation to clients"})
            .field(&NetworkIdentityComponent::relevancy_radius, "relevancy_radius",
                   {.min = 0, .max = 10000, .speed = 0.5f, .tooltip = "Only clients whose player is this close receive updates (0 = everyone)"})
            .field(&NetworkIdentityComponent::priority, "priority", {.min = 0.01f, .max = 100, .speed = 0.05f, .tooltip = "Share of the snapshot budget"})
            .commit();
    });
}

const char* to_string(Role r) noexcept {
    switch (r) {
    case Role::Offline: return "offline";
    case Role::Server: return "server";
    case Role::Client: return "client";
    }
    return "offline";
}

NetContext* context_of(Scene& scene) {
    const auto* b = scene.world().resource<Binding>();
    return b ? b->context : nullptr;
}

void bind(Scene& scene, NetContext* context) {
    if (auto* b = scene.world().resource<Binding>()) b->context = context;
    else scene.world().set_resource<Binding>().context = context;
}

Role role_of(Scene& scene) {
    const auto* c = context_of(scene);
    return c ? c->role() : Role::Offline;
}

std::uint16_t owner_of(Scene& scene, Entity e) {
    for (Entity x = e; !x.is_null() && scene.world().alive(x); x = scene.parent(x))
        if (const auto* s = scene.world().get<NetState>(x); s && s->net_id) return s->owner;
    return kServerId;
}

namespace {
// Nearest ancestor-or-self that a session tracks.
const NetState* tracked_state(Scene& scene, Entity e) {
    for (Entity x = e; !x.is_null() && scene.world().alive(x); x = scene.parent(x))
        if (const auto* s = scene.world().get<NetState>(x); s && s->net_id) return s;
    return nullptr;
}
} // namespace

bool has_authority(Scene& scene, Entity) { return role_of(scene) != Role::Client; }

bool is_local(Scene& scene, Entity e) {
    switch (role_of(scene)) {
    case Role::Offline: return true;
    case Role::Server: return owner_of(scene, e) == kServerId;
    case Role::Client: {
        const NetState* s = tracked_state(scene, e);
        return s && s->local;
    }
    }
    return false;
}

Status set_var(Scene& scene, Entity e, std::string_view key, const json::Value& value) {
    if (!scene.world().alive(e)) return make_error(ErrorCode::InvalidArgument, "set_net: entity is not alive");
    if (!has_authority(scene, e)) return make_error(ErrorCode::InvalidArgument, "set_net: only the server may change network variables");
    if (!valid_name(key)) return make_error(ErrorCode::InvalidArgument, "set_net: key must be 1-64 printable characters");
    if (value.is_array() || value.is_object()) {
        if (json::stringify(value, false).size() > kMaxVarBytes)
            return make_error(ErrorCode::LimitExceeded, "set_net: value larger than " + std::to_string(kMaxVarBytes) + " bytes");
    } else if (value.is_string() && value.as_string().size() > kMaxVarBytes) {
        return make_error(ErrorCode::LimitExceeded, "set_net: value larger than " + std::to_string(kMaxVarBytes) + " bytes");
    }
    NetState& s = state_of(scene, e);
    if (value.is_null()) {
        json::Value out = json::Value::object();
        for (const auto& [k, v] : s.vars.as_object())
            if (k != key) out.set(k, v);
        s.vars = std::move(out);
    } else {
        s.vars.set(key, value);
    }
    return {};
}

json::Value get_var(Scene& scene, Entity e, std::string_view key) {
    if (!scene.world().alive(e)) return {};
    const auto* s = scene.world().get<NetState>(e);
    if (!s) return {};
    const json::Value* v = s->vars.find(key);
    return v ? *v : json::Value{};
}

std::string make_game_id(std::string_view project, std::string_view scene_path) {
    return std::string(project) + "|" + std::string(scene_path);
}

// =================================================================================================
// Server
// =================================================================================================

struct NetServer::Impl {
    std::unique_ptr<Transport> transport;
    ServerOptions options;
    Scene* scene{nullptr};
    bool begun{false};
    std::vector<Template> templates;
    std::uint32_t tick{0};
    std::uint32_t next_net_id{1};
    std::uint16_t next_client_id{1};
    double now{0};

    struct Tracked {
        Entity e;
        std::uint8_t kind{kSceneEntity};
        Uuid uuid; // scene entity uuid, or the template uuid for players
        std::uint16_t owner{kServerId};
        std::string name;
        json::Value sent_vars = json::Value::object();
    };
    std::map<std::uint32_t, Tracked> tracked;

    struct Client {
        PeerId peer{0};
        std::uint16_t id{0};
        std::string name;
        bool welcomed{false};
        Entity player;
        std::deque<InputFrame> inputs;
        std::uint32_t last_received_seq{0}, last_processed_seq{0};
        RemoteInput input;
        double echo_time{0}, echo_received{0};
        float snapshot_accum{0};
        float tokens{0};
        std::uint32_t violations{0};
        struct Sent {
            QState q; // last sent state (non-delta mode)
            double at{-1e9};
            float priority{0};
        };
        std::unordered_map<std::uint32_t, Sent> sent;
        // Delta snapshots: what each recent snapshot told the client, and the newest it confirmed.
        std::uint32_t snapshot_seq{0}, snapshot_ack{0};
        std::deque<std::pair<std::uint32_t, View>> views;
    };
    std::map<PeerId, Client> clients;
    std::vector<NetMessage> inbox;
    std::vector<TransportEvent> events;
    Meter out_meter, in_meter;
    std::uint64_t snapshots_sent{0}, delta_snapshots{0};
    std::uint32_t last_snapshot_bytes{0}, last_deferred{0};
    std::uint64_t rejected{0};
    NetServer* owner_ctx{nullptr};

    void send(const Client& c, Channel ch, const ByteWriter& w) {
        transport->send(c.peer, ch, w.data());
        out_meter.add(w.size());
    }
    void broadcast(Channel ch, const ByteWriter& w) {
        for (const auto& [peer, c] : clients)
            if (c.welcomed) send(c, ch, w);
    }
    void violation(Client& c, const char* what) {
        ++rejected;
        ++c.violations;
        VICTUS_LOG_WARN("net", "client {} ({}): {}", c.id, c.name, what);
        if (c.violations > options.max_violations) {
            VICTUS_LOG_WARN("net", "kicking client {} after {} violations", c.id, c.violations);
            ByteWriter w;
            w.u8(kServerGoodbye);
            w.str("too many invalid messages");
            send(c, Channel::Reliable, w);
            transport->disconnect(c.peer);
            drop_client(c.peer);
        }
    }

    void begin(Scene& s) {
        scene = &s;
        begun = true;
        templates = park_templates(s);
        auto& routing = s.world().set_resource<gameplay::InputRouting>();
        routing.route = [this](Entity e, const gameplay::InputSource* fallback) -> const gameplay::InputSource* {
            if (!scene || !scene->world().alive(e)) return fallback;
            const NetState* st = tracked_state(*scene, e);
            if (!st || st->owner == kServerId) return fallback;
            for (auto& [peer, c] : clients)
                if (c.id == st->owner) return &c.input;
            static const RemoteInput none;
            return &none;
        };
    }

    void write_spawn(ByteWriter& w, std::uint32_t id, const Tracked& t) {
        w.u8(kSpawn);
        w.varuint(id);
        w.u8(t.kind);
        write_uuid(w, t.uuid);
        w.u16(t.owner);
        const auto* tr = scene->world().get<Transform>(t.e);
        w.vec3(tr ? tr->position : math::Vec3{});
        w.quat(tr ? tr->rotation : math::Quat{});
        w.str(t.name);
    }
    void write_vars(ByteWriter& w, std::uint32_t id, const json::Value& vars) {
        w.u8(kVars);
        w.varuint(id);
        const auto bytes = encode_json(vars);
        w.str(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    void write_roster(ByteWriter& w) {
        w.u8(kRoster);
        std::uint32_t n = 0;
        for (const auto& [p, c] : clients) n += c.welcomed;
        w.varuint(n);
        for (const auto& [p, c] : clients) {
            if (!c.welcomed) continue;
            w.u16(c.id);
            w.str(c.name);
            const auto* st = scene->world().alive(c.player) ? scene->world().get<NetState>(c.player) : nullptr;
            w.varuint(st ? st->net_id : 0);
        }
    }

    std::uint32_t track(Entity e, std::uint8_t kind, Uuid uuid, std::uint16_t owner, std::string name) {
        const std::uint32_t id = next_net_id++;
        NetState& st = state_of(*scene, e);
        st.net_id = id;
        st.owner = owner;
        tracked[id] = Tracked{e, kind, uuid, owner, std::move(name), json::Value::object()};
        return id;
    }

    // Tracks scene entities with a NetworkIdentity that have no net id yet and announces them.
    void scan_new() {
        auto& w = scene->world();
        std::vector<Entity> fresh;
        w.each<NetworkIdentityComponent>([&](Entity e, NetworkIdentityComponent& id) {
            if (id.player_template) return;
            const auto* st = w.get<NetState>(e);
            if (!st || st->net_id == 0) fresh.push_back(e);
        });
        for (const Entity e : fresh) {
            const auto* nm = w.get<scene::Name>(e);
            const std::uint32_t id = track(e, kSceneEntity, scene->uuid(e), kServerId, nm ? nm->value : std::string());
            ByteWriter sw;
            write_spawn(sw, id, tracked[id]);
            broadcast(Channel::Reliable, sw);
        }
    }

    void handle_hello(Client& c, ByteReader& r) {
        const std::uint32_t magic = r.u32();
        const std::uint32_t version = r.u32();
        const std::string game = r.str(kMaxGameId);
        const std::string name = r.str(kMaxPlayerName * 4);
        auto reject = [&](const std::string& why) {
            ByteWriter w;
            w.u8(kReject);
            w.str(why);
            send(c, Channel::Reliable, w);
            VICTUS_LOG_INFO("net", "rejected connection: {}", why);
            ++rejected;
            const PeerId peer = c.peer;
            transport->disconnect(peer);
            clients.erase(peer); // `c` is gone: callers return right after
        };
        if (c.welcomed) return violation(c, "duplicate hello");
        if (!r.done() || magic != kMagic) return reject("not a VICTUS client");
        if (version != kProtocolVersion)
            return reject("protocol version " + std::to_string(version) + " != server " + std::to_string(kProtocolVersion));
        if (game != options.game_id) return reject("different game or scene (server runs '" + options.game_id + "')");
        std::size_t active = 0;
        for (const auto& [p, other] : clients) active += other.welcomed;
        if (active >= options.max_clients) return reject("server full");
        scan_new(); // so the new client receives every scene entity before the roster

        c.id = next_client_id++;
        if (next_client_id == kAllClients) next_client_id = 1;
        c.name = clean_player_name(name);
        c.welcomed = true;
        c.tokens = options.message_rate;
        ByteWriter w;
        w.u8(kWelcome);
        w.u16(c.id);
        w.u32(tick);
        send(c, Channel::Reliable, w);
        // Existing world state, then this client's player (announced to everyone).
        for (const auto& [id, t] : tracked) {
            if (!scene->world().alive(t.e)) continue;
            ByteWriter sw;
            write_spawn(sw, id, t);
            send(c, Channel::Reliable, sw);
            if (const auto* st = scene->world().get<NetState>(t.e); st && st->vars.size()) {
                ByteWriter vw;
                write_vars(vw, id, st->vars);
                send(c, Channel::Reliable, vw);
            }
        }
        for (std::size_t i = 0; i < templates.size(); ++i) {
            const Template& tpl = templates[i];
            auto e = instantiate(*scene, tpl, c.name);
            if (!e) {
                VICTUS_LOG_ERROR("net", "cannot spawn player: {}", e.error().message);
                continue;
            }
            // Spread players on a ring around the template position.
            if (auto* tr = scene->world().get<Transform>(*e)) {
                const float a = static_cast<float>(c.id) * 2.39996f; // golden angle
                tr->position = tpl.position + math::Vec3{std::sin(a), 0, std::cos(a)} * options.spawn_spacing;
            }
            if (i == 0) c.player = *e;
            const std::uint32_t id = track(*e, kPlayer, tpl.uuid, c.id, c.name);
            ByteWriter sw;
            write_spawn(sw, id, tracked[id]);
            broadcast(Channel::Reliable, sw);
        }
        ByteWriter rw;
        write_roster(rw);
        broadcast(Channel::Reliable, rw);
        inbox.push_back({c.id, "client_joined", json::Value(static_cast<double>(c.id))});
        VICTUS_LOG_INFO("net", "client {} '{}' joined ({} connected)", c.id, c.name, active + 1);
    }

    void handle_input(Client& c, ByteReader& r) {
        const std::uint32_t newest = r.u32();
        const std::uint8_t count = r.u8();
        if (count == 0 || count > kMaxInputFrames) return violation(c, "bad input frame count");
        std::array<InputFrame, kMaxInputFrames> frames{};
        for (std::uint8_t i = 0; i < count; ++i) {
            frames[i].seq = newest - i;
            frames[i].keys.lo = r.u64();
            frames[i].keys.hi = r.u64();
            frames[i].mouse = r.u8();
            const std::uint8_t pad = r.u8();
            if (pad > 1) return violation(c, "malformed input");
            if (pad) {
                frames[i].pad = true;
                frames[i].pad_buttons = static_cast<std::uint16_t>(r.u16() & ((1u << gameplay::kGamepadButtonCount) - 1));
                for (auto& a : frames[i].pad_axes) a = static_cast<std::int16_t>(r.u16());
            }
        }
        const double client_time = r.f64();
        const std::uint32_t snapshot_ack = r.u32();
        if (!r.done()) return violation(c, "malformed input");
        if (snapshot_ack > c.snapshot_seq) return violation(c, "acknowledged an unsent snapshot");
        c.snapshot_ack = std::max(c.snapshot_ack, snapshot_ack);
        constexpr std::uint64_t kValidHi = kKeyCount > 64 ? (1ull << (kKeyCount - 64)) - 1 : 0;
        if (newest <= c.last_received_seq) return; // stale or duplicated packet
        c.echo_time = client_time;
        c.echo_received = now;
        for (int i = count - 1; i >= 0; --i) {
            InputFrame f = frames[static_cast<std::size_t>(i)];
            if (f.seq <= c.last_received_seq) continue;
            f.keys.hi &= kValidHi;
            c.inputs.push_back(f);
        }
        c.last_received_seq = newest;
        while (c.inputs.size() > 64) c.inputs.pop_front();
    }

    void handle_message(Client& c, ByteReader& r) {
        const std::string name = r.str(kMaxNameBytes);
        const std::string text = r.str(kMaxMessageBytes);
        if (!r.done() || !valid_name(name)) return violation(c, "malformed message");
        if (c.tokens < 1.0f) return violation(c, "message rate limit exceeded");
        c.tokens -= 1.0f;
        auto v = decode_json(text, kMaxMessageBytes);
        if (!v) return violation(c, "message payload is not valid JSON");
        inbox.push_back({c.id, name, std::move(v).value()});
    }

    void drop_client(PeerId peer) {
        const auto it = clients.find(peer);
        if (it == clients.end()) return;
        Client& c = it->second;
        if (c.welcomed) {
            VICTUS_LOG_INFO("net", "client {} '{}' left", c.id, c.name);
            inbox.push_back({c.id, "client_left", json::Value(static_cast<double>(c.id))});
            // Its player entities go away (despawns are broadcast in replicate()).
            for (auto& [id, t] : tracked)
                if (t.owner == c.id && scene->world().alive(t.e)) scene->destroy(t.e);
        }
        const bool was_welcomed = c.welcomed;
        clients.erase(it);
        if (was_welcomed) {
            ByteWriter rw;
            write_roster(rw);
            broadcast(Channel::Reliable, rw);
        }
    }

    void handle_event(TransportEvent& ev) {
        switch (ev.type) {
        case TransportEvent::Type::Connected: clients[ev.peer].peer = ev.peer; break;
        case TransportEvent::Type::Disconnected: drop_client(ev.peer); break;
        case TransportEvent::Type::Received: {
            in_meter.add(ev.data.size());
            auto it = clients.find(ev.peer);
            if (it == clients.end()) break;
            Client& c = it->second;
            ByteReader r(ev.data);
            const std::uint8_t type = r.u8();
            if (type == kHello) handle_hello(c, r);
            else if (!c.welcomed) violation(c, "message before hello");
            else if (type == kInput && ev.channel == Channel::Unreliable) handle_input(c, r);
            else if (type == kClientMessage && ev.channel == Channel::Reliable) handle_message(c, r);
            else if (type == kClientGoodbye) {
                transport->disconnect(c.peer);
                drop_client(c.peer);
            } else violation(c, "unexpected message type");
            break;
        }
        }
    }

    void receive(Scene& s, double t) {
        VICTUS_PROFILE_SCOPE("net.server.receive");
        now = t;
        if (!begun || scene != &s) begin(s);
        events = std::move(pending);
        pending.clear();
        transport->poll(now, events);
        for (auto& ev : events) handle_event(ev);
        // One input frame per client per tick; catch up (merging) when the buffer grows.
        for (auto& [peer, c] : clients) {
            if (!c.welcomed) continue;
            c.tokens = std::min(options.message_rate, c.tokens + options.message_rate / 60.0f);
            if (c.inputs.empty()) {
                c.input.hold();
                continue;
            }
            InputFrame f = c.inputs.front();
            c.inputs.pop_front();
            while (c.inputs.size() > options.input_buffer) {
                const InputFrame next = c.inputs.front();
                c.inputs.pop_front();
                f.keys = f.keys | next.keys; // keep presses from skipped frames
                f.mouse |= next.mouse;
                f.pad_buttons |= next.pad_buttons; // newest axes, merged buttons
                f.pad = f.pad || next.pad;
                f.pad_axes = next.pad_axes;
                f.seq = next.seq;
            }
            c.input.apply(f);
            c.last_processed_seq = f.seq;
        }
        ++tick;
        in_meter.tick(now);
    }

    void replicate(Scene& s, double t, float dt) {
        VICTUS_PROFILE_SCOPE("net.server.replicate");
        now = t;
        if (!begun || scene != &s) begin(s);
        auto& w = s.world();
        // Despawns.
        for (auto it = tracked.begin(); it != tracked.end();) {
            if (w.alive(it->second.e) && w.get<NetState>(it->second.e)) {
                ++it;
                continue;
            }
            ByteWriter dw;
            dw.u8(kDespawn);
            dw.varuint(it->first);
            broadcast(Channel::Reliable, dw);
            for (auto& [peer, c] : clients) c.sent.erase(it->first);
            it = tracked.erase(it);
        }
        scan_new();
        // Variable changes (reliable, only changed keys; null = removed).
        for (auto& [id, tr] : tracked) {
            const auto* st = w.get<NetState>(tr.e);
            if (!st || st->vars == tr.sent_vars) continue;
            json::Value diff = json::Value::object();
            for (const auto& [k, v] : st->vars.as_object()) {
                const json::Value* old = tr.sent_vars.find(k);
                if (!old || !(*old == v)) diff.set(k, v);
            }
            for (const auto& [k, v] : tr.sent_vars.as_object())
                if (!st->vars.find(k)) diff.set(k, json::Value{});
            tr.sent_vars = st->vars;
            ByteWriter vw;
            write_vars(vw, id, diff);
            broadcast(Channel::Reliable, vw);
        }
        // Snapshots per client within the byte budget, highest accumulated priority first.
        const float interval = 1.0f / std::max(1.0f, options.snapshot_rate);
        for (auto& [peer, c] : clients) {
            if (!c.welcomed) continue;
            c.snapshot_accum += dt;
            if (c.snapshot_accum + 1e-5f < interval) continue;
            c.snapshot_accum = std::fmod(c.snapshot_accum, interval);
            const Transform* viewer = w.alive(c.player) ? w.get<Transform>(c.player) : nullptr;
            // Baseline: the newest snapshot the client acknowledged that we still remember.
            const View* baseline = nullptr;
            if (options.delta_snapshots && c.snapshot_ack)
                for (const auto& [seq, v] : c.views)
                    if (seq == c.snapshot_ack) baseline = &v;
            const std::uint32_t baseline_seq = baseline ? c.snapshot_ack : 0;
            static const View kEmpty;
            const View& base = baseline ? *baseline : kEmpty;
            struct Candidate {
                std::uint32_t id;
                QState q;
                float priority;
            };
            std::vector<Candidate> cands;
            for (auto& [id, tr] : tracked) {
                const auto* ni = w.get<NetworkIdentityComponent>(tr.e);
                const auto* xf = w.get<Transform>(tr.e);
                if (!xf || (ni && !ni->replicate_transform)) continue;
                if (ni && ni->relevancy_radius > 0 && viewer && math::length(xf->position - viewer->position) > ni->relevancy_radius) continue;
                auto& sent = c.sent[id];
                const QState q = quantize(xf->position, xf->rotation);
                if (options.delta_snapshots) {
                    // Anything the client has not confirmed yet is (re)sent until it is acknowledged.
                    const QState* known = find_state(base, id);
                    if (known && *known == q) continue;
                } else {
                    if (sent.q == q && now - sent.at < kResendInterval) continue; // unchanged, resent each second
                }
                sent.priority += (ni ? ni->priority : 1.0f) * (tr.owner == c.id ? 4.0f : 1.0f);
                cands.push_back({id, q, sent.priority});
            }
            std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
                return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
            });
            ByteWriter sw;
            sw.u8(kSnapshot);
            sw.u32(tick);
            sw.u32(c.last_processed_seq);
            sw.f64(c.echo_time);
            sw.f64(now - c.echo_received);
            const std::uint32_t seq = ++c.snapshot_seq;
            sw.u32(seq);
            sw.u32(baseline_seq);
            View view;
            if (options.delta_snapshots) {
                view.reserve(base.size() + cands.size());
                for (const auto& e : base)
                    if (tracked.count(e.first)) view.push_back(e); // despawned entities drop out
            }
            const std::size_t count_at = sw.size();
            sw.u16(0);
            std::uint16_t n = 0;
            std::uint32_t deferred = 0;
            for (const auto& cd : cands) {
                constexpr std::size_t kEntryMax = 6 + 3 * 10 + 4 * 3;
                if (sw.size() + kEntryMax > options.snapshot_budget || n == 0xFFFF) {
                    ++deferred;
                    continue;
                }
                static const QState kZero{};
                const QState* known = find_state(base, cd.id);
                write_entry(sw, cd.id, cd.q, known ? *known : kZero);
                if (options.delta_snapshots) put_state(view, cd.id, cd.q);
                auto& sent = c.sent[cd.id];
                sent.q = cd.q;
                sent.at = now;
                sent.priority = 0;
                ++n;
            }
            sw.patch_u16(count_at, n);
            send(c, Channel::Unreliable, sw);
            if (options.delta_snapshots) {
                c.views.emplace_back(seq, std::move(view));
                while (c.views.size() > kServerViews) c.views.pop_front();
            }
            ++snapshots_sent;
            delta_snapshots += baseline_seq != 0;
            last_snapshot_bytes = static_cast<std::uint32_t>(sw.size());
            last_deferred = deferred;
        }
        out_meter.tick(now);
        // Flush what this tick produced now; anything that arrives meanwhile is handled next tick.
        transport->poll(now, pending);
    }
    std::vector<TransportEvent> pending;
};

NetServer::NetServer(std::unique_ptr<Transport> transport, ServerOptions options) : impl_(std::make_unique<Impl>()) {
    register_components();
    impl_->transport = std::move(transport);
    impl_->options = std::move(options);
    impl_->owner_ctx = this;
}

NetServer::~NetServer() {
    if (impl_->scene) {
        if (context_of(*impl_->scene) == this) bind(*impl_->scene, nullptr);
        if (auto* r = impl_->scene->world().resource<gameplay::InputRouting>()) r->route = nullptr;
    }
}

void NetServer::install(gameplay::Simulation& sim) {
    NetServer* self = this;
    sim.add_system("net.receive", gameplay::Phase::PreUpdate, [self](Scene& s, const gameplay::FrameTime& t) {
        if (context_of(s) != self) bind(s, self);
        self->receive(s, t.time);
    });
    sim.add_system("net.send", gameplay::Phase::PostUpdate,
                   [self](Scene& s, const gameplay::FrameTime& t) { self->replicate(s, t.time, t.dt); });
}

void NetServer::receive(Scene& scene, double now) {
    if (context_of(scene) != this) bind(scene, this);
    impl_->receive(scene, now);
}

void NetServer::replicate(Scene& scene, double now, float dt) { impl_->replicate(scene, now, dt); }

void NetServer::shutdown(double now) {
    Impl& m = *impl_;
    ByteWriter w;
    w.u8(kServerGoodbye);
    w.str("server shutting down");
    for (auto& [peer, c] : m.clients) {
        m.send(c, Channel::Reliable, w);
        m.transport->disconnect(peer);
    }
    m.clients.clear();
    m.events.clear();
    m.transport->poll(now, m.events);
}

Status NetServer::send(std::string_view name, const json::Value& value, std::uint16_t to) {
    if (!valid_name(name)) return make_error(ErrorCode::InvalidArgument, "net.send: name must be 1-64 printable characters");
    const auto bytes = encode_json(value);
    if (bytes.size() > kMaxMessageBytes) return make_error(ErrorCode::LimitExceeded, "net.send: payload larger than 1024 bytes");
    ByteWriter w;
    w.u8(kServerMessage);
    w.u16(kServerId);
    w.str(name);
    w.str(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    for (auto& [peer, c] : impl_->clients)
        if (c.welcomed && (to == kAllClients || c.id == to)) impl_->send(c, Channel::Reliable, w);
    return {};
}

std::vector<NetMessage> NetServer::take_messages() { return std::exchange(impl_->inbox, {}); }

std::vector<ClientInfo> NetServer::clients() const {
    std::vector<ClientInfo> out;
    for (const auto& [peer, c] : impl_->clients) {
        if (!c.welcomed) continue;
        out.push_back({c.id, c.name, c.player, impl_->transport->stats(peer).rtt_ms});
    }
    return out;
}

NetDiagnostics NetServer::diagnostics() const {
    const Impl& m = *impl_;
    NetDiagnostics d;
    d.role = Role::Server;
    d.connected = true;
    d.tick = m.tick;
    for (const auto& [p, c] : m.clients) d.clients += c.welcomed;
    d.entities = static_cast<std::uint32_t>(m.tracked.size());
    d.send_kbps = m.out_meter.kbps;
    d.receive_kbps = m.in_meter.kbps;
    d.bytes_sent = m.out_meter.total;
    d.bytes_received = m.in_meter.total;
    d.snapshots_sent = m.snapshots_sent;
    d.delta_snapshots = m.delta_snapshots;
    d.snapshot_bytes = m.last_snapshot_bytes;
    d.entities_deferred = m.last_deferred;
    d.rejected_messages = m.rejected;
    d.status = "serving " + std::to_string(d.clients) + " client(s) over " + std::string(m.transport->name());
    return d;
}

// =================================================================================================
// Client
// =================================================================================================

struct NetClient::Impl {
    std::unique_ptr<Transport> transport;
    ClientOptions options;
    Scene* scene{nullptr};
    bool begun{false};
    std::vector<Template> templates;
    enum class State { Idle, Connecting, Handshake, Connected, Failed } state{State::Idle};
    std::string status{"not connected"};
    PeerId server{0};
    std::uint16_t id{0};
    double now{0}, last_heard{0};
    std::uint32_t seq{0};
    std::uint32_t server_tick{0};

    struct Sample {
        double t;
        math::Vec3 p;
        math::Quat q;
    };
    struct Remote {
        Entity e;
        std::uint16_t owner{kServerId};
        bool local{false};
        std::deque<Sample> samples;
    };
    std::map<std::uint32_t, Remote> entities;
    Entity local_player;

    struct History {
        std::uint32_t seq;
        InputFrame frame;
        math::Vec3 predicted;
        bool recorded{false};
    };
    std::deque<History> history;
    math::Vec3 pending_correction;
    float last_error{0};
    std::uint64_t corrections{0};

    double clock_offset{0}; // estimated server time - local time
    bool clock_valid{false};
    float rtt_ms{0};
    std::uint32_t last_snapshot_tick{0};
    std::uint64_t snapshots_received{0}, delta_snapshots{0}, undecodable_snapshots{0};
    std::deque<std::pair<std::uint32_t, View>> views; // decoded snapshots usable as baselines
    std::uint32_t snapshot_ack{0};                     // newest decoded snapshot (sent with input)
    std::uint32_t last_snapshot_bytes{0};
    std::vector<ClientInfo> roster;
    bool synced{false};
    std::vector<NetMessage> inbox;
    std::vector<TransportEvent> events;
    Meter out_meter, in_meter;
    static constexpr float kTickDt = 1.0f / 60.0f;

    void send(Channel ch, const ByteWriter& w) {
        if (!server) return;
        transport->send(server, ch, w.data());
        out_meter.add(w.size());
    }
    void fail(std::string why) {
        if (state == State::Failed) return;
        state = State::Failed;
        status = std::move(why);
        VICTUS_LOG_WARN("net", "{}", status);
    }

    void begin(Scene& s) {
        scene = &s;
        begun = true;
        templates = park_templates(s);
        auto& routing = s.world().set_resource<gameplay::InputRouting>();
        routing.route = [this](Entity e, const gameplay::InputSource* fallback) -> const gameplay::InputSource* {
            if (!scene || !scene->world().alive(e)) return fallback;
            const NetState* st = tracked_state(*scene, e);
            if (!st || st->local) return fallback;
            return nullptr; // remote players and server objects get no local input
        };
        if (state == State::Idle) {
            auto peer = transport->connect(options.host, options.port);
            if (!peer) return fail("cannot connect: " + peer.error().message);
            server = *peer;
            state = State::Connecting;
            status = "connecting to " + options.host + ":" + std::to_string(options.port);
            last_heard = now;
        }
    }

    const Template* find_template(Uuid u) const {
        for (const auto& t : templates)
            if (t.uuid == u) return &t;
        return nullptr;
    }

    void handle_spawn(ByteReader& r) {
        const auto net_id = static_cast<std::uint32_t>(r.varuint());
        const std::uint8_t kind = r.u8();
        const Uuid uuid = read_uuid(r);
        const std::uint16_t owner = r.u16();
        const math::Vec3 p = r.vec3();
        const math::Quat q = r.quat();
        const std::string name = r.str(kMaxNameBytes * 2);
        if (!r.done() || net_id == 0) return;
        if (entities.count(net_id)) return;
        Entity e;
        if (kind == kSceneEntity) {
            e = scene->find(uuid);
            if (e.is_null()) {
                VICTUS_LOG_WARN("net", "server entity {} ('{}') is not in the local scene", uuid.to_string(), name);
                return;
            }
        } else if (kind == kPlayer) {
            const Template* t = find_template(uuid);
            if (!t) {
                VICTUS_LOG_WARN("net", "unknown player template {}", uuid.to_string());
                return;
            }
            auto made = instantiate(*scene, *t, name);
            if (!made) return;
            e = *made;
        } else {
            return;
        }
        if (auto* tr = scene->world().get<Transform>(e)) {
            tr->position = p;
            tr->rotation = q;
        }
        NetState& st = state_of(*scene, e);
        st.net_id = net_id;
        st.owner = owner;
        st.local = owner == id && owner != kServerId;
        st.proxy = !st.local;
        if (st.proxy) make_proxy(*scene, e);
        if (st.local) {
            local_player = e;
            history.clear();
            pending_correction = {};
        }
        entities[net_id] = Remote{e, owner, st.local, {}};
    }

    void handle_despawn(ByteReader& r) {
        const auto net_id = static_cast<std::uint32_t>(r.varuint());
        if (!r.done()) return;
        // Net ids are never reused, so the id can leave every baseline (the server drops it too).
        for (auto& [s, v] : views)
            v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) { return e.first == net_id; }), v.end());
        const auto it = entities.find(net_id);
        if (it == entities.end()) return;
        if (scene->world().alive(it->second.e)) scene->destroy(it->second.e);
        if (it->second.e == local_player) local_player = {};
        entities.erase(it);
    }

    void handle_vars(ByteReader& r) {
        const auto net_id = static_cast<std::uint32_t>(r.varuint());
        const std::string text = r.str(64 * 1024);
        if (!r.done()) return;
        const auto it = entities.find(net_id);
        if (it == entities.end() || !scene->world().alive(it->second.e)) return;
        auto v = decode_json(text, 64 * 1024);
        if (!v || !v->is_object()) return;
        NetState& st = state_of(*scene, it->second.e);
        json::Value merged = json::Value::object();
        for (const auto& [k, val] : st.vars.as_object())
            if (!v->find(k)) merged.set(k, val);
        for (const auto& [k, val] : v->as_object())
            if (!val.is_null()) merged.set(k, val);
        st.vars = std::move(merged);
    }

    void handle_roster(ByteReader& r) {
        const std::uint64_t n = r.varuint();
        if (n > 4096) return;
        std::vector<ClientInfo> list;
        for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
            ClientInfo c;
            c.id = r.u16();
            c.name = r.str(kMaxPlayerName * 4);
            const auto net_id = static_cast<std::uint32_t>(r.varuint());
            if (const auto it = entities.find(net_id); it != entities.end()) c.player = it->second.e;
            list.push_back(std::move(c));
        }
        if (!r.done()) return;
        roster = std::move(list);
        if (!synced) {
            // The first roster follows the server's full world state: networked scene entities the
            // server did not announce no longer exist there (e.g. destroyed before we joined).
            synced = true;
            std::vector<Entity> stale;
            scene->world().each<NetworkIdentityComponent>([&](Entity e, NetworkIdentityComponent&) {
                const auto* st = scene->world().get<NetState>(e);
                if (!st || st->net_id == 0) stale.push_back(e);
            });
            for (const Entity e : stale)
                if (scene->world().alive(e)) scene->destroy(e);
        }
    }

    void handle_snapshot(ByteReader& r) {
        const std::uint32_t tick = r.u32();
        const std::uint32_t ack = r.u32();
        const double echo = r.f64();
        const double held = r.f64();
        const std::uint32_t snap_seq = r.u32();
        const std::uint32_t baseline_seq = r.u32();
        const std::uint16_t n = r.u16();
        if (!r.ok()) return;
        static const View kEmpty;
        const View* base = &kEmpty;
        if (baseline_seq) {
            base = nullptr;
            for (const auto& [s, v] : views)
                if (s == baseline_seq) base = &v;
            if (!base) { // baseline forgotten (should not happen): wait for a decodable snapshot
                ++undecodable_snapshots;
                return;
            }
        }
        struct Entry {
            std::uint32_t id;
            math::Vec3 p;
            math::Quat q;
        };
        std::vector<Entry> list;
        list.reserve(n);
        View view = *base;
        static const QState kZero{};
        for (std::uint16_t i = 0; i < n && r.ok(); ++i) {
            const std::uint64_t head = r.varuint();
            if (head >> 34) r.fail(); // id beyond 32 bits
            const auto id = static_cast<std::uint32_t>(head >> 2);
            const QState* known = find_state(*base, id);
            QState st = known ? *known : kZero;
            if (head & kHasPosition)
                for (std::size_t k = 0; k < 3; ++k)
                    st.p[k] = static_cast<std::int32_t>(std::clamp<std::int64_t>(st.p[k] + unzigzag(r.varuint()), INT32_MIN, INT32_MAX));
            if (head & kHasRotation)
                for (auto& c : st.q) c = static_cast<std::uint16_t>(std::clamp<std::int64_t>(c + unzigzag(r.varuint()), 0, 65535));
            put_state(view, id, st);
            list.push_back({id, position_of(st), rotation_of(st)});
        }
        if (!r.done()) return;
        ++snapshots_received;
        delta_snapshots += baseline_seq != 0;
        if (snap_seq > snapshot_ack) {
            snapshot_ack = snap_seq;
            views.emplace_back(snap_seq, std::move(view));
            while (views.size() > kClientViews) views.pop_front();
        }
        if (echo > 0) {
            const float sample_rtt = static_cast<float>(std::max(0.0, (now - echo - held)) * 1000.0);
            rtt_ms = rtt_ms == 0 ? sample_rtt : rtt_ms + (sample_rtt - rtt_ms) * 0.1f;
        }
        if (tick < last_snapshot_tick) return; // reordered: older than what we have
        last_snapshot_tick = tick;
        server_tick = tick;
        const double server_time = static_cast<double>(tick) * kTickDt;
        const double target = server_time + rtt_ms * 0.5e-3 - now;
        if (!clock_valid || std::abs(target - clock_offset) > 0.5) clock_offset = target;
        else clock_offset += (target - clock_offset) * 0.05;
        clock_valid = true;
        for (const Entry& en : list) {
            const auto it = entities.find(en.id);
            if (it == entities.end()) continue;
            Remote& rm = it->second;
            if (rm.local) {
                reconcile(ack, en.p);
                continue;
            }
            rm.samples.push_back({server_time, en.p, en.q});
            while (rm.samples.size() > 32) rm.samples.pop_front();
        }
    }

    // Compares the server's position after input `ack` with what we predicted for that input.
    void reconcile(std::uint32_t ack, math::Vec3 server_pos) {
        const auto it = std::find_if(history.begin(), history.end(), [&](const History& h) { return h.seq == ack && h.recorded; });
        if (it == history.end()) return;
        const math::Vec3 error = server_pos - it->predicted;
        last_error = math::length(error);
        history.erase(history.begin(), it + 1);
        if (last_error < 0.01f) {
            pending_correction = {};
            return;
        }
        ++corrections;
        if (last_error > options.snap_distance) {
            if (auto* tr = scene->world().get<Transform>(local_player)) tr->position = tr->position + error;
            for (auto& h : history) h.predicted = h.predicted + error;
            pending_correction = {};
            return;
        }
        pending_correction = error; // blended in over the next frames
    }

    void handle(const TransportEvent& ev) {
        switch (ev.type) {
        case TransportEvent::Type::Connected: {
            if (ev.peer != server) return;
            state = State::Handshake;
            status = "handshaking";
            ByteWriter w;
            w.u8(kHello);
            w.u32(kMagic);
            w.u32(kProtocolVersion);
            w.str(options.game_id);
            w.str(options.name);
            send(Channel::Reliable, w);
            break;
        }
        case TransportEvent::Type::Disconnected:
            if (ev.peer != server) return;
            server = 0;
            if (state != State::Failed) fail(state == State::Connected ? "disconnected from server" : "could not reach the server");
            break;
        case TransportEvent::Type::Received: {
            if (ev.peer != server) return;
            in_meter.add(ev.data.size());
            last_heard = now;
            ByteReader r(ev.data);
            const std::uint8_t type = r.u8();
            switch (type) {
            case kWelcome:
                id = r.u16();
                server_tick = r.u32();
                if (r.done()) {
                    state = State::Connected;
                    status = "connected as client " + std::to_string(id);
                    VICTUS_LOG_INFO("net", "{}", status);
                }
                break;
            case kReject: fail("rejected by server: " + r.str(512)); break;
            case kServerGoodbye: fail("server closed the connection: " + r.str(512)); break;
            case kSpawn: handle_spawn(r); break;
            case kDespawn: handle_despawn(r); break;
            case kVars: handle_vars(r); break;
            case kRoster: handle_roster(r); break;
            case kSnapshot:
                last_snapshot_bytes = static_cast<std::uint32_t>(ev.data.size());
                handle_snapshot(r);
                break;
            case kServerMessage: {
                const std::uint16_t sender = r.u16();
                const std::string name = r.str(kMaxNameBytes);
                const std::string text = r.str(kMaxMessageBytes);
                if (!r.done()) break;
                if (auto v = decode_json(text, kMaxMessageBytes)) inbox.push_back({sender, name, std::move(v).value()});
                break;
            }
            default: break; // newer server feature or garbage: ignore
            }
            break;
        }
        }
    }

    void receive(Scene& s, double t, const gameplay::InputSource* input) {
        VICTUS_PROFILE_SCOPE("net.client.receive");
        now = t;
        if (!begun || scene != &s) begin(s);
        events.clear();
        transport->poll(now, events);
        for (const auto& ev : events) handle(ev);
        if (state != State::Failed && state != State::Idle && now - last_heard > options.timeout) {
            fail("connection timed out");
            if (server) transport->disconnect(server);
            server = 0;
        }
        if (state != State::Connected) return;
        // Sample this tick's input, remember it for reconciliation and send the newest frames.
        const InputFrame f = sample(input, ++seq);
        history.push_back({seq, f, {}, false});
        while (history.size() > 256) history.pop_front();
        ByteWriter w;
        w.u8(kInput);
        w.u32(seq);
        const std::size_t count = std::min<std::size_t>(
            history.size(), std::clamp<std::size_t>(static_cast<std::size_t>(options.input_redundancy), 1, kMaxInputFrames));
        w.u8(static_cast<std::uint8_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            const InputFrame& h = history[history.size() - 1 - i].frame;
            w.u64(h.keys.lo);
            w.u64(h.keys.hi);
            w.u8(h.mouse);
            w.u8(h.pad ? 1 : 0);
            if (h.pad) {
                w.u16(h.pad_buttons);
                for (const auto a : h.pad_axes) w.u16(static_cast<std::uint16_t>(a));
            }
        }
        w.f64(now);
        w.u32(snapshot_ack);
        send(Channel::Unreliable, w);
    }

    void replicate(Scene& s, double t, float dt) {
        VICTUS_PROFILE_SCOPE("net.client.replicate");
        now = t;
        if (!begun || scene != &s) begin(s);
        auto& w = s.world();
        // Local player: blend away the prediction error and record the prediction for this input.
        if (w.alive(local_player)) {
            if (auto* tr = w.get<Transform>(local_player)) {
                if (math::length(pending_correction) > 0) {
                    const float k = 1.0f - std::exp(-options.correction_rate * std::max(dt, 1e-4f));
                    const math::Vec3 step = pending_correction * k;
                    tr->position = tr->position + step;
                    for (auto& h : history)
                        if (h.recorded) h.predicted = h.predicted + step;
                    pending_correction = pending_correction - step;
                    if (math::length(pending_correction) < 1e-3f) pending_correction = {};
                }
                if (!history.empty() && !history.back().recorded) {
                    history.back().predicted = tr->position;
                    history.back().recorded = true;
                }
            }
        }
        // Everything else: interpolate between snapshots `interpolation_delay` in the past.
        const double render = now + clock_offset - options.interpolation_delay;
        for (auto& entry : entities) {
            auto& rm = entry.second;
            if (rm.local || rm.samples.empty() || !w.alive(rm.e)) continue;
            auto* tr = w.get<Transform>(rm.e);
            if (!tr) continue;
            const auto& sm = rm.samples;
            if (render <= sm.front().t) {
                tr->position = sm.front().p;
                tr->rotation = sm.front().q;
                continue;
            }
            std::size_t i = 1;
            while (i < sm.size() && sm[i].t < render) ++i;
            if (i == sm.size()) {
                // Past the newest sample: extrapolate for up to 0.1 s (a late packet), then ease
                // back onto the newest sample over the next 0.2 s. Entities stop being sent
                // when they stop changing, so a lasting gap means "it stopped there".
                const Sample& b = sm.back();
                if (sm.size() >= 2) {
                    const Sample& a = sm[sm.size() - 2];
                    const double span = b.t - a.t;
                    const double late = render - b.t;
                    const double ahead = late <= 0.1 ? late : std::max(0.0, 0.1 - (late - 0.1) * 0.5);
                    if (span > 1e-6 && ahead > 0) {
                        tr->position = b.p + (b.p - a.p) * static_cast<float>(ahead / span);
                        tr->rotation = b.q;
                        continue;
                    }
                }
                tr->position = b.p;
                tr->rotation = b.q;
                continue;
            }
            const Sample& a = sm[i - 1];
            const Sample& b = sm[i];
            const float u = static_cast<float>(std::clamp((render - a.t) / std::max(1e-6, b.t - a.t), 0.0, 1.0));
            tr->position = math::lerp(a.p, b.p, u);
            tr->rotation = math::slerp(a.q, b.q, u);
        }
        out_meter.tick(now);
        in_meter.tick(now);
        events.clear();
        transport->poll(now, events); // flush the input sent this tick
        for (const auto& ev : events) handle(ev);
    }
};

NetClient::NetClient(std::unique_ptr<Transport> transport, ClientOptions options) : impl_(std::make_unique<Impl>()) {
    register_components();
    impl_->transport = std::move(transport);
    impl_->options = std::move(options);
    impl_->options.name = clean_player_name(impl_->options.name);
}

NetClient::~NetClient() {
    if (impl_->server) impl_->transport->disconnect(impl_->server);
    std::vector<TransportEvent> ignore;
    impl_->transport->poll(impl_->now, ignore);
    if (impl_->scene) {
        if (context_of(*impl_->scene) == this) bind(*impl_->scene, nullptr);
        if (auto* r = impl_->scene->world().resource<gameplay::InputRouting>()) r->route = nullptr;
    }
}

void NetClient::install(gameplay::Simulation& sim) {
    NetClient* self = this;
    gameplay::Simulation* simp = &sim;
    sim.add_system("net.receive", gameplay::Phase::PreUpdate, [self, simp](Scene& s, const gameplay::FrameTime& t) {
        if (context_of(s) != self) bind(s, self);
        self->receive(s, t.time, simp->input);
    });
    sim.add_system("net.send", gameplay::Phase::PostUpdate,
                   [self](Scene& s, const gameplay::FrameTime& t) { self->replicate(s, t.time, t.dt); });
}

void NetClient::receive(Scene& scene, double now, const gameplay::InputSource* input) {
    if (context_of(scene) != this) bind(scene, this);
    impl_->receive(scene, now, input);
}
void NetClient::replicate(Scene& scene, double now, float dt) { impl_->replicate(scene, now, dt); }

void NetClient::disconnect(double now) {
    Impl& m = *impl_;
    if (m.server) {
        ByteWriter w;
        w.u8(kClientGoodbye);
        m.send(Channel::Reliable, w);
        m.transport->disconnect(m.server);
        m.server = 0;
    }
    m.events.clear();
    m.transport->poll(now, m.events);
    if (m.state != Impl::State::Failed) {
        m.state = Impl::State::Failed;
        m.status = "disconnected";
    }
}

std::uint16_t NetClient::local_id() const noexcept { return impl_->id; }
bool NetClient::connected() const noexcept { return impl_->state == Impl::State::Connected; }
bool NetClient::failed() const noexcept { return impl_->state == Impl::State::Failed; }
Entity NetClient::local_player() const noexcept { return impl_->local_player; }

Status NetClient::send(std::string_view name, const json::Value& value, std::uint16_t) {
    if (!connected()) return make_error(ErrorCode::NetworkError, "net.send: not connected");
    if (!valid_name(name)) return make_error(ErrorCode::InvalidArgument, "net.send: name must be 1-64 printable characters");
    const auto bytes = encode_json(value);
    if (bytes.size() > kMaxMessageBytes) return make_error(ErrorCode::LimitExceeded, "net.send: payload larger than 1024 bytes");
    ByteWriter w;
    w.u8(kClientMessage);
    w.str(name);
    w.str(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    impl_->send(Channel::Reliable, w);
    return {};
}

std::vector<NetMessage> NetClient::take_messages() { return std::exchange(impl_->inbox, {}); }

std::vector<ClientInfo> NetClient::clients() const {
    auto list = impl_->roster;
    for (auto& c : list)
        if (c.id == impl_->id) c.rtt_ms = impl_->rtt_ms;
    return list;
}

NetDiagnostics NetClient::diagnostics() const {
    const Impl& m = *impl_;
    NetDiagnostics d;
    d.role = Role::Client;
    d.connected = connected();
    d.tick = m.server_tick;
    d.clients = static_cast<std::uint32_t>(m.roster.size());
    d.entities = static_cast<std::uint32_t>(m.entities.size());
    d.rtt_ms = m.rtt_ms;
    d.send_kbps = m.out_meter.kbps;
    d.receive_kbps = m.in_meter.kbps;
    d.bytes_sent = m.out_meter.total;
    d.bytes_received = m.in_meter.total;
    d.snapshots_received = m.snapshots_received;
    d.delta_snapshots = m.delta_snapshots;
    d.snapshot_bytes = m.last_snapshot_bytes;
    d.prediction_error = m.last_error;
    d.corrections = m.corrections;
    d.status = m.status;
    return d;
}

} // namespace victus::net
