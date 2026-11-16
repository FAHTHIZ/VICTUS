// SPDX-License-Identifier: Apache-2.0
// Transports: UDP via ENet, the in-memory network and the link-conditions decorator.
#include "victus/net/net.hpp"

#include "victus/core/log.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace victus::net {

static_assert(std::endian::native == std::endian::little, "VICTUS wire format assumes a little-endian host");

namespace {

// ---- ENet -------------------------------------------------------------------------------------

std::mutex g_enet_mutex;
int g_enet_users = 0;

bool enet_acquire() {
    std::lock_guard lock(g_enet_mutex);
    if (g_enet_users == 0 && enet_initialize() != 0) return false;
    ++g_enet_users;
    return true;
}

void enet_release() {
    std::lock_guard lock(g_enet_mutex);
    if (--g_enet_users == 0) enet_deinitialize();
}

constexpr std::size_t kChannels = 2;

class UdpTransport final : public Transport {
public:
    UdpTransport(ENetHost* host, const UdpOptions& o) : host_(host), options_(o) {}
    ~UdpTransport() override {
        for (auto& [id, peer] : peers_) enet_peer_disconnect_now(peer, 0);
        enet_host_flush(host_);
        enet_host_destroy(host_);
        enet_release();
    }

    Result<PeerId> connect(std::string_view host, std::uint16_t port) override {
        ENetAddress address{};
        const std::string h(host);
        if (enet_address_set_host(&address, h.c_str()) != 0)
            return make_error(ErrorCode::NetworkError, "cannot resolve host '" + h + "'");
        address.port = port;
        ENetPeer* peer = enet_host_connect(host_, &address, kChannels, 0);
        if (!peer) return make_error(ErrorCode::NetworkError, "no free connection slot");
        const auto timeout = static_cast<enet_uint32>(options_.connect_timeout * 1000.0);
        enet_peer_timeout(peer, 0, timeout, timeout);
        return track(peer);
    }

    void send(PeerId id, Channel channel, std::span<const std::uint8_t> data) override {
        const auto it = peers_.find(id);
        if (it == peers_.end()) return;
        if (data.size() > max_payload()) {
            VICTUS_LOG_WARN("net", "dropping {} byte packet (limit {})", data.size(), max_payload());
            return;
        }
        const enet_uint32 flags = channel == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
        if (!packet) return;
        if (enet_peer_send(it->second, static_cast<enet_uint8>(channel), packet) != 0) enet_packet_destroy(packet);
        auto& s = stats_[id];
        s.bytes_sent += data.size();
        ++s.packets_sent;
    }

    void disconnect(PeerId id) override {
        const auto it = peers_.find(id);
        if (it == peers_.end()) return;
        enet_peer_disconnect_later(it->second, 0); // after queued reliable data
        it->second->data = nullptr;
        peers_.erase(it);
        stats_.erase(id);
    }

    void poll(double, std::vector<TransportEvent>& out) override {
        ENetEvent ev;
        while (enet_host_service(host_, &ev, 0) > 0) {
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                PeerId id = id_of(ev.peer);
                if (!id) id = track(ev.peer); // incoming connection (server)
                out.push_back({TransportEvent::Type::Connected, id, Channel::Reliable, {}});
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                const PeerId id = id_of(ev.peer);
                if (id) {
                    peers_.erase(id);
                    stats_.erase(id);
                    ev.peer->data = nullptr;
                    out.push_back({TransportEvent::Type::Disconnected, id, Channel::Reliable, {}});
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const PeerId id = id_of(ev.peer);
                if (id && ev.channelID < kChannels) {
                    TransportEvent e{TransportEvent::Type::Received, id, static_cast<Channel>(ev.channelID), {}};
                    e.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                    auto& s = stats_[id];
                    s.bytes_received += e.data.size();
                    ++s.packets_received;
                    out.push_back(std::move(e));
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_NONE: break;
            }
        }
        enet_host_flush(host_);
    }

    PeerStats stats(PeerId id) const override {
        PeerStats s;
        if (const auto it = stats_.find(id); it != stats_.end()) s = it->second;
        if (const auto it = peers_.find(id); it != peers_.end()) s.rtt_ms = static_cast<float>(it->second->roundTripTime);
        return s;
    }
    std::string_view name() const override { return "udp"; }
    std::size_t max_payload() const override { return 256 * 1024; }

private:
    PeerId track(ENetPeer* peer) {
        const PeerId id = next_id_++;
        // ENet's user data slot carries our id (no allocation, cleared on disconnect).
        peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        peers_[id] = peer;
        return id;
    }
    static PeerId id_of(const ENetPeer* peer) { return static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(peer->data)); }

    ENetHost* host_;
    UdpOptions options_;
    std::unordered_map<PeerId, ENetPeer*> peers_;
    std::unordered_map<PeerId, PeerStats> stats_;
    PeerId next_id_{1};
};

} // namespace

Result<std::unique_ptr<Transport>> create_udp_transport(const UdpOptions& o) {
    if (!enet_acquire()) return make_error(ErrorCode::NetworkError, "ENet initialization failed");
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = o.port;
    if (!o.bind_address.empty() && enet_address_set_host(&address, o.bind_address.c_str()) != 0) {
        enet_release();
        return make_error(ErrorCode::NetworkError, "cannot resolve bind address '" + o.bind_address + "'");
    }
    const bool server = o.port != 0;
    ENetHost* host = enet_host_create(server ? &address : nullptr, server ? o.max_peers : 1, kChannels, 0, 0);
    if (!host) {
        enet_release();
        return make_error(ErrorCode::NetworkError,
                          server ? "cannot listen on UDP port " + std::to_string(o.port) + " (in use?)" : "cannot create UDP socket");
    }
    return std::unique_ptr<Transport>(new UdpTransport(host, o));
}

// ---- in-memory network ------------------------------------------------------------------------

struct MemoryNetwork::State {
    struct Endpoint;
    struct Link {
        std::weak_ptr<Endpoint> remote;
        PeerId remote_peer{0};
        PeerStats stats;
    };
    struct Endpoint {
        std::deque<TransportEvent> inbox;
        std::map<PeerId, Link> links;
        PeerId next_peer{1};
        std::uint16_t port{0};
    };
    std::map<std::uint16_t, std::weak_ptr<Endpoint>> listeners;
};

namespace {

class MemoryTransport final : public Transport {
public:
    using State = MemoryNetwork::State;
    MemoryTransport(std::shared_ptr<State> net, std::uint16_t port) : net_(std::move(net)), self_(std::make_shared<State::Endpoint>()) {
        self_->port = port;
        if (port) net_->listeners[port] = self_;
    }
    ~MemoryTransport() override {
        for (auto& [id, link] : self_->links)
            if (auto r = link.remote.lock()) {
                r->links.erase(link.remote_peer);
                r->inbox.push_back({TransportEvent::Type::Disconnected, link.remote_peer, Channel::Reliable, {}});
            }
        if (self_->port) net_->listeners.erase(self_->port);
    }
    Result<PeerId> connect(std::string_view, std::uint16_t port) override {
        const auto it = net_->listeners.find(port);
        auto server = it == net_->listeners.end() ? nullptr : it->second.lock();
        const PeerId mine = self_->next_peer++;
        if (!server) {
            // Like a UDP connect to a closed port: the attempt fails asynchronously.
            self_->inbox.push_back({TransportEvent::Type::Disconnected, mine, Channel::Reliable, {}});
            return mine;
        }
        const PeerId theirs = server->next_peer++;
        self_->links[mine] = {server, theirs, {}};
        server->links[theirs] = {self_, mine, {}};
        server->inbox.push_back({TransportEvent::Type::Connected, theirs, Channel::Reliable, {}});
        self_->inbox.push_back({TransportEvent::Type::Connected, mine, Channel::Reliable, {}});
        return mine;
    }
    void send(PeerId id, Channel channel, std::span<const std::uint8_t> data) override {
        const auto it = self_->links.find(id);
        if (it == self_->links.end()) return;
        auto r = it->second.remote.lock();
        if (!r) return;
        TransportEvent e{TransportEvent::Type::Received, it->second.remote_peer, channel, {data.begin(), data.end()}};
        it->second.stats.bytes_sent += data.size();
        ++it->second.stats.packets_sent;
        if (const auto back = r->links.find(it->second.remote_peer); back != r->links.end()) {
            back->second.stats.bytes_received += data.size();
            ++back->second.stats.packets_received;
        }
        r->inbox.push_back(std::move(e));
    }
    void disconnect(PeerId id) override {
        const auto it = self_->links.find(id);
        if (it == self_->links.end()) return;
        if (auto r = it->second.remote.lock()) {
            r->links.erase(it->second.remote_peer);
            r->inbox.push_back({TransportEvent::Type::Disconnected, it->second.remote_peer, Channel::Reliable, {}});
        }
        self_->links.erase(it);
    }
    void poll(double, std::vector<TransportEvent>& out) override {
        while (!self_->inbox.empty()) {
            out.push_back(std::move(self_->inbox.front()));
            self_->inbox.pop_front();
        }
    }
    PeerStats stats(PeerId id) const override {
        const auto it = self_->links.find(id);
        return it == self_->links.end() ? PeerStats{} : it->second.stats;
    }
    std::string_view name() const override { return "memory"; }

private:
    std::shared_ptr<State> net_;
    std::shared_ptr<State::Endpoint> self_;
};

// ---- link conditions --------------------------------------------------------------------------

class ConditionedTransport final : public Transport {
public:
    ConditionedTransport(std::unique_ptr<Transport> inner, const LinkConditions& c) : inner_(std::move(inner)), c_(c), rng_(c.seed ? c.seed : 1) {}
    Result<PeerId> connect(std::string_view host, std::uint16_t port) override { return inner_->connect(host, port); }
    void send(PeerId peer, Channel channel, std::span<const std::uint8_t> data) override {
        double delay = c_.latency_ms * 1e-3 + c_.jitter_ms * 1e-3 * uniform();
        const bool lost = c_.loss > 0 && uniform() < c_.loss;
        if (channel == Channel::Unreliable && lost) {
            ++dropped_[peer];
            return;
        }
        if (channel == Channel::Reliable && lost) delay += 2.0 * (c_.latency_ms * 1e-3) + 0.03; // retransmit round trip
        double release = now_ + delay;
        if (c_.bandwidth_kbps > 0) {
            // Serialization delay through a shared outgoing pipe.
            const double seconds = static_cast<double>(data.size() + 28) * 8.0 / (c_.bandwidth_kbps * 1000.0);
            pipe_free_ = std::max(pipe_free_, now_) + seconds;
            if (channel == Channel::Unreliable && pipe_free_ - now_ > 1.0) { // queue overflow
                pipe_free_ -= seconds;
                ++dropped_[peer];
                return;
            }
            release = std::max(release, pipe_free_);
        }
        if (channel == Channel::Reliable) {
            double& last = reliable_release_[peer];
            release = std::max(release, last); // in order
            last = release;
        }
        queue_.push({release, order_++, peer, channel, {data.begin(), data.end()}});
    }
    void disconnect(PeerId peer) override {
        flush(1e300, peer); // queued reliable data still goes out first
        inner_->disconnect(peer);
        reliable_release_.erase(peer);
    }
    void poll(double now, std::vector<TransportEvent>& out) override {
        now_ = now;
        flush(now, 0);
        inner_->poll(now, out);
    }
    PeerStats stats(PeerId peer) const override {
        PeerStats s = inner_->stats(peer);
        if (const auto it = dropped_.find(peer); it != dropped_.end()) s.packets_dropped = it->second;
        return s;
    }
    std::string_view name() const override { return inner_->name(); }
    std::size_t max_payload() const override { return inner_->max_payload(); }

private:
    struct Pending {
        double release;
        std::uint64_t order;
        PeerId peer;
        Channel channel;
        std::vector<std::uint8_t> data;
        bool operator>(const Pending& o) const { return release != o.release ? release > o.release : order > o.order; }
    };
    // Sends everything due by `now`, or (only != 0) everything queued for that peer.
    void flush(double now, PeerId only) {
        if (!only) {
            while (!queue_.empty() && queue_.top().release <= now) {
                inner_->send(queue_.top().peer, queue_.top().channel, queue_.top().data);
                queue_.pop();
            }
            return;
        }
        std::vector<Pending> keep;
        while (!queue_.empty()) {
            Pending p = queue_.top();
            queue_.pop();
            if (p.peer == only) inner_->send(p.peer, p.channel, p.data);
            else keep.push_back(std::move(p));
        }
        for (auto& p : keep) queue_.push(std::move(p));
    }
    double uniform() {
        // splitmix64: deterministic for a given seed.
        std::uint64_t z = (rng_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
    std::unique_ptr<Transport> inner_;
    LinkConditions c_;
    std::uint64_t rng_;
    double now_{0};
    double pipe_free_{0};
    std::uint64_t order_{0};
    std::priority_queue<Pending, std::vector<Pending>, std::greater<>> queue_;
    std::unordered_map<PeerId, double> reliable_release_;
    std::unordered_map<PeerId, std::uint64_t> dropped_;
};

} // namespace

MemoryNetwork::MemoryNetwork() : state_(std::make_shared<State>()) {}
MemoryNetwork::~MemoryNetwork() = default;

Result<std::unique_ptr<Transport>> MemoryNetwork::listen(std::uint16_t port) {
    if (port == 0) return make_error(ErrorCode::InvalidArgument, "memory network: port 0 is reserved for clients");
    if (const auto it = state_->listeners.find(port); it != state_->listeners.end() && !it->second.expired())
        return make_error(ErrorCode::AlreadyExists, "memory network: port " + std::to_string(port) + " is taken");
    return std::unique_ptr<Transport>(new MemoryTransport(state_, port));
}

std::unique_ptr<Transport> MemoryNetwork::endpoint() { return std::make_unique<MemoryTransport>(state_, 0); }

std::unique_ptr<Transport> with_conditions(std::unique_ptr<Transport> inner, const LinkConditions& c) {
    if (!c.active()) return inner;
    return std::make_unique<ConditionedTransport>(std::move(inner), c);
}

Result<LinkConditions> parse_conditions(std::string_view text) {
    LinkConditions c;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(',', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view item = text.substr(start, end - start);
        start = end + 1;
        if (item.empty()) continue;
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) return make_error(ErrorCode::InvalidArgument, "expected key=value in '" + std::string(item) + "'");
        const std::string_view key = item.substr(0, eq);
        const std::string value(item.substr(eq + 1));
        char* stop = nullptr;
        const double v = std::strtod(value.c_str(), &stop);
        if (value.empty() || *stop != '\0' || v < 0) return make_error(ErrorCode::InvalidArgument, "bad value in '" + std::string(item) + "'");
        if (key == "latency") c.latency_ms = static_cast<float>(v);
        else if (key == "jitter") c.jitter_ms = static_cast<float>(v);
        else if (key == "loss") {
            if (v > 1) return make_error(ErrorCode::InvalidArgument, "loss must be 0..1");
            c.loss = static_cast<float>(v);
        } else if (key == "bandwidth") c.bandwidth_kbps = static_cast<float>(v);
        else if (key == "seed") c.seed = static_cast<std::uint64_t>(v);
        else return make_error(ErrorCode::InvalidArgument, "unknown condition '" + std::string(key) + "' (latency, jitter, loss, bandwidth, seed)");
    }
    return c;
}

Result<std::pair<std::string, std::uint16_t>> parse_address(std::string_view text) {
    if (text.empty()) return make_error(ErrorCode::InvalidArgument, "empty address");
    std::string host(text);
    std::uint16_t port = kDefaultPort;
    if (const std::size_t colon = text.rfind(':'); colon != std::string_view::npos) {
        host = std::string(text.substr(0, colon));
        const std::string_view p = text.substr(colon + 1);
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(p.data(), p.data() + p.size(), value);
        if (ec != std::errc{} || ptr != p.data() + p.size() || value == 0 || value > 65535)
            return make_error(ErrorCode::InvalidArgument, "bad port in '" + std::string(text) + "'");
        port = static_cast<std::uint16_t>(value);
    }
    if (host.empty()) host = "127.0.0.1";
    return std::pair{host, port};
}

} // namespace victus::net
