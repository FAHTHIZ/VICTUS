// SPDX-License-Identifier: Apache-2.0
// Encrypted, authenticated sessions over any Transport (ADR-0028, docs/NETWORKING_SPEC.md).
//
// Handshake (Noise "NX"-style, reliable channel):
//   C -> S  0xA1 "VSEC" version ec                      ec = client ephemeral X25519 key
//   S -> C  0xA2 es ss mac                              es = server ephemeral, ss = server static
//   both:   h     = BLAKE2b("VICTUS-SECURE-1" ec es ss)
//           c2s   = BLAKE2b(key h, X25519(e,e') || X25519(ec,ss) || "c2s")   s2c likewise
//           mac   = empty XChaCha20-Poly1305 message under s2c (proves the server holds ss)
//   C -> S  data{auth, password}   S -> C  data{accept} | data{reject, reason} + disconnect
// Data packets: 0xA3 counter(u64 LE) ciphertext(kind byte + payload) mac(16); the nonce is the
// counter and channel, the header and channel are authenticated. Counters are per direction and
// channel: reliable ones must increase, unreliable ones pass a 64-packet replay window.
#include "victus/net/net.hpp"

#include "victus/core/log.hpp"

#include <monocypher.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <unordered_map>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <bcrypt.h>
#elif defined(__APPLE__)
#    include <stdlib.h>
#else
#    include <sys/random.h>
#endif
#if !defined(_WIN32)
#    include <sys/stat.h>
#endif

namespace victus::net {

Status random_bytes(std::span<std::uint8_t> out) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return make_error(ErrorCode::IoError, "BCryptGenRandom failed");
#elif defined(__APPLE__)
    arc4random_buf(out.data(), out.size());
#else
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return make_error(ErrorCode::IoError, "getrandom failed");
        }
        done += static_cast<std::size_t>(n);
    }
#endif
    return {};
}

Key public_key_of(const Key& secret) {
    Key pub{};
    crypto_x25519_public_key(pub.data(), secret.data());
    return pub;
}

Result<KeyPair> generate_key_pair() {
    KeyPair kp;
    if (auto st = random_bytes(kp.secret); !st) return std::move(st).error();
    kp.public_key = public_key_of(kp.secret);
    return kp;
}

std::string key_to_hex(const Key& key) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const auto b : key) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

Result<Key> key_from_hex(std::string_view hex) {
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) hex.remove_suffix(1);
    if (hex.size() != 64) return make_error(ErrorCode::InvalidArgument, "a key is 64 hexadecimal characters");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Key k{};
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return make_error(ErrorCode::InvalidArgument, "a key is 64 hexadecimal characters");
        k[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return k;
}

Result<KeyPair> load_or_create_key(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto k = key_from_hex(text);
        crypto_wipe(text.data(), text.size());
        if (!k) return make_error(ErrorCode::InvalidArgument, path.string() + ": " + k.error().message);
        return KeyPair{*k, public_key_of(*k)};
    }
    auto kp = generate_key_pair();
    if (!kp) return kp;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return make_error(ErrorCode::IoError, "cannot write " + path.string());
#if !defined(_WIN32)
        ::chmod(path.string().c_str(), S_IRUSR | S_IWUSR);
#endif
        out << key_to_hex(kp->secret) << '\n';
        if (!out) return make_error(ErrorCode::IoError, "cannot write " + path.string());
    }
    return kp;
}

namespace {

constexpr std::uint8_t kClientHello = 0xA1;
constexpr std::uint8_t kServerHello = 0xA2;
constexpr std::uint8_t kData = 0xA3;
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kClientHelloSize = 1 + 4 + 1 + 32;
constexpr std::size_t kServerHelloSize = 1 + 32 + 32 + 16;
constexpr std::size_t kHeader = 1 + 8;
// Encrypted payload kinds.
constexpr std::uint8_t kApp = 0, kAuth = 1, kAccept = 2, kReject = 3;
constexpr std::size_t kMaxPassword = 256;
static_assert(kSecureOverhead == kHeader + 1 + 16);

void put_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

class SecureTransport final : public Transport {
public:
    SecureTransport(std::unique_ptr<Transport> inner, SecureOptions o) : inner_(std::move(inner)), o_(std::move(o)) {
        if (o_.server_secret) static_public_ = public_key_of(*o_.server_secret);
    }
    ~SecureTransport() override {
        for (auto& [id, p] : peers_) p.wipe();
        if (o_.server_secret) crypto_wipe(o_.server_secret->data(), 32);
        crypto_wipe(o_.password.data(), o_.password.size());
    }

    Result<PeerId> connect(std::string_view host, std::uint16_t port) override {
        if (o_.server_secret) return make_error(ErrorCode::InvalidArgument, "a server transport cannot connect");
        auto id = inner_->connect(host, port);
        if (id) {
            Peer& p = peers_[*id];
            p.state = State::Connecting; // the inner transport times out connects itself
        }
        return id;
    }

    void send(PeerId peer, Channel channel, std::span<const std::uint8_t> data) override {
        const auto it = peers_.find(peer);
        if (it == peers_.end() || it->second.state != State::Open) return; // not (yet) an open session
        seal(peer, it->second, channel, kApp, data);
    }

    void disconnect(PeerId peer) override {
        if (const auto it = peers_.find(peer); it != peers_.end()) {
            it->second.wipe();
            peers_.erase(it);
        }
        inner_->disconnect(peer);
    }

    void poll(double now, std::vector<TransportEvent>& out) override {
        now_ = now;
        scratch_.clear();
        inner_->poll(now, scratch_);
        for (auto& ev : scratch_) handle(ev, out);
        // Handshakes that do not finish in time are abandoned.
        for (auto it = peers_.begin(); it != peers_.end();) {
            Peer& p = it->second;
            if (p.state != State::Open && p.started >= 0 && now - p.started > o_.handshake_timeout) {
                const PeerId id = it->first;
                VICTUS_LOG_WARN("net", "secure handshake with peer {} timed out", id);
                const bool client = !o_.server_secret;
                p.wipe();
                it = peers_.erase(it);
                inner_->disconnect(id);
                if (client) out.push_back({TransportEvent::Type::Disconnected, id, Channel::Reliable, {}, "secure handshake timed out"});
                continue;
            }
            ++it;
        }
    }

    PeerStats stats(PeerId peer) const override {
        PeerStats s = inner_->stats(peer);
        if (const auto it = peers_.find(peer); it != peers_.end()) {
            s.encrypted = it->second.state == State::Open;
            if (!o_.server_secret && it->second.has_remote_static) s.remote_key = key_to_hex(it->second.remote_static);
            s.packets_rejected = it->second.rejected;
        }
        return s;
    }
    std::string_view name() const override { return inner_->name(); }
    std::size_t max_payload() const override {
        const std::size_t m = inner_->max_payload();
        return m > kSecureOverhead ? m - kSecureOverhead : 0;
    }
    bool encrypted() const override { return true; }

private:
    enum class State : std::uint8_t {
        Connecting,     // client: transport connecting
        AwaitServer,    // client: hello sent
        AwaitAccept,    // client: password sent
        AwaitHello,     // server: waiting for the client hello
        AwaitAuth,      // server: keys agreed, waiting for the password
        Open,
    };
    struct Peer {
        State state{State::AwaitHello};
        double started{-1};
        Key e_secret{}, e_public{};
        Key k_send{}, k_recv{};
        Key remote_static{};
        bool has_remote_static{false};
        std::uint64_t send_counter[2]{0, 0};
        std::uint64_t recv_max[2]{0, 0};
        bool recv_any[2]{false, false};
        std::uint64_t window[2]{0, 0}; // bit i: counter recv_max - i was seen
        std::uint64_t rejected{0};
        void wipe() {
            crypto_wipe(e_secret.data(), 32);
            crypto_wipe(k_send.data(), 32);
            crypto_wipe(k_recv.data(), 32);
        }
    };

    static void nonce_for(std::uint8_t nonce[24], std::uint64_t counter, Channel ch) {
        std::memset(nonce, 0, 24);
        put_u64(nonce, counter);
        nonce[8] = static_cast<std::uint8_t>(ch);
    }

    void seal(PeerId id, Peer& p, Channel ch, std::uint8_t kind, std::span<const std::uint8_t> data) {
        if (data.size() > max_payload()) {
            VICTUS_LOG_WARN("net", "dropping {}-byte packet (limit {})", data.size(), max_payload());
            return;
        }
        const auto c = static_cast<std::size_t>(ch);
        const std::uint64_t counter = p.send_counter[c]++;
        std::vector<std::uint8_t> pkt(kHeader + 1 + data.size() + 16);
        pkt[0] = kData;
        put_u64(pkt.data() + 1, counter);
        std::vector<std::uint8_t> plain(1 + data.size());
        plain[0] = kind;
        if (!data.empty()) std::memcpy(plain.data() + 1, data.data(), data.size());
        std::uint8_t nonce[24];
        nonce_for(nonce, counter, ch);
        std::uint8_t ad[kHeader + 1];
        std::memcpy(ad, pkt.data(), kHeader);
        ad[kHeader] = static_cast<std::uint8_t>(ch);
        crypto_aead_lock(pkt.data() + kHeader, pkt.data() + pkt.size() - 16, p.k_send.data(), nonce, ad, sizeof ad, plain.data(),
                         plain.size());
        crypto_wipe(plain.data(), plain.size());
        inner_->send(id, ch, pkt);
    }

    // Decrypts a data packet; false for forgeries, corruption and replays.
    bool open(Peer& p, Channel ch, const std::vector<std::uint8_t>& pkt, std::vector<std::uint8_t>& plain) {
        if (pkt.size() < kSecureOverhead || pkt[0] != kData) return false;
        const auto c = static_cast<std::size_t>(ch);
        const std::uint64_t counter = get_u64(pkt.data() + 1);
        if (p.recv_any[c]) {
            if (ch == Channel::Reliable && counter <= p.recv_max[c]) return false;
            if (ch == Channel::Unreliable && counter <= p.recv_max[c]) {
                const std::uint64_t age = p.recv_max[c] - counter;
                if (age >= 64 || ((p.window[c] >> age) & 1u)) return false;
            }
        }
        std::uint8_t nonce[24];
        nonce_for(nonce, counter, ch);
        std::uint8_t ad[kHeader + 1];
        std::memcpy(ad, pkt.data(), kHeader);
        ad[kHeader] = static_cast<std::uint8_t>(ch);
        const std::size_t n = pkt.size() - kHeader - 16;
        plain.resize(n);
        if (crypto_aead_unlock(plain.data(), pkt.data() + pkt.size() - 16, p.k_recv.data(), nonce, ad, sizeof ad,
                               pkt.data() + kHeader, n) != 0)
            return false;
        // Authentic: advance the replay state.
        if (!p.recv_any[c] || counter > p.recv_max[c]) {
            const std::uint64_t shift = p.recv_any[c] ? counter - p.recv_max[c] : 0;
            p.window[c] = shift >= 64 ? 0 : p.window[c] << shift;
            p.window[c] |= 1u;
            p.recv_max[c] = counter;
            p.recv_any[c] = true;
        } else {
            p.window[c] |= std::uint64_t{1} << (p.recv_max[c] - counter);
        }
        return !plain.empty();
    }

    // Derives both directions' keys and the transcript hash the confirmation authenticates.
    void derive(Peer& p, const Key& ec, const Key& es, const Key& ss, const std::uint8_t dh1[32], const std::uint8_t dh2[32],
                bool server) {
        std::uint8_t transcript[15 + 96];
        std::memcpy(transcript, "VICTUS-SECURE-1", 15);
        std::memcpy(transcript + 15, ec.data(), 32);
        std::memcpy(transcript + 47, es.data(), 32);
        std::memcpy(transcript + 79, ss.data(), 32);
        std::uint8_t h[32];
        crypto_blake2b(h, 32, transcript, sizeof transcript);
        std::uint8_t ikm[67];
        std::memcpy(ikm, dh1, 32);
        std::memcpy(ikm + 32, dh2, 32);
        Key c2s{}, s2c{};
        std::memcpy(ikm + 64, "c2s", 3);
        crypto_blake2b_keyed(c2s.data(), 32, h, 32, ikm, sizeof ikm);
        std::memcpy(ikm + 64, "s2c", 3);
        crypto_blake2b_keyed(s2c.data(), 32, h, 32, ikm, sizeof ikm);
        crypto_wipe(ikm, sizeof ikm);
        p.k_send = server ? s2c : c2s;
        p.k_recv = server ? c2s : s2c;
        crypto_wipe(c2s.data(), 32);
        crypto_wipe(s2c.data(), 32);
        std::memcpy(transcript_hash_.data(), h, 32);
    }

    void end_client(PeerId id, Peer& p, std::string reason, std::vector<TransportEvent>& out) {
        VICTUS_LOG_WARN("net", "secure session: {}", reason);
        p.wipe();
        peers_.erase(id);
        inner_->disconnect(id);
        out.push_back({TransportEvent::Type::Disconnected, id, Channel::Reliable, {}, std::move(reason)});
    }
    void end_server(PeerId id, Peer& p, std::string_view why) {
        VICTUS_LOG_WARN("net", "secure session with peer {} refused: {}", id, why);
        p.wipe();
        peers_.erase(id);
        inner_->disconnect(id);
    }

    void handle(TransportEvent& ev, std::vector<TransportEvent>& out) {
        const bool server = o_.server_secret.has_value();
        switch (ev.type) {
        case TransportEvent::Type::Connected: {
            Peer& p = peers_[ev.peer];
            p.started = now_;
            if (server) {
                p.state = State::AwaitHello;
                return;
            }
            // Client: start the handshake with a fresh ephemeral key.
            auto kp = generate_key_pair();
            if (!kp) return end_client(ev.peer, p, kp.error().message, out);
            p.e_secret = kp->secret;
            p.e_public = kp->public_key;
            crypto_wipe(kp->secret.data(), 32);
            std::vector<std::uint8_t> hello(kClientHelloSize);
            hello[0] = kClientHello;
            std::memcpy(hello.data() + 1, "VSEC", 4);
            hello[5] = kVersion;
            std::memcpy(hello.data() + 6, p.e_public.data(), 32);
            inner_->send(ev.peer, Channel::Reliable, hello);
            p.state = State::AwaitServer;
            return;
        }
        case TransportEvent::Type::Disconnected: {
            const auto it = peers_.find(ev.peer);
            if (it == peers_.end()) return;
            const bool was_open = it->second.state == State::Open;
            it->second.wipe();
            peers_.erase(it);
            // The session layer only knows open sessions (and, on clients, the pending connect).
            if (was_open || !server) out.push_back(std::move(ev));
            return;
        }
        case TransportEvent::Type::Received: break;
        }
        const auto it = peers_.find(ev.peer);
        if (it == peers_.end()) return;
        Peer& p = it->second;
        const PeerId id = ev.peer;
        const auto& d = ev.data;
        switch (p.state) {
        case State::AwaitHello: {
            if (ev.channel != Channel::Reliable || d.size() != kClientHelloSize || d[0] != kClientHello ||
                std::memcmp(d.data() + 1, "VSEC", 4) != 0)
                return end_server(id, p, "not a secure client (connect with encryption enabled)");
            if (d[5] != kVersion) return end_server(id, p, "unsupported secure session version");
            Key ec{};
            std::memcpy(ec.data(), d.data() + 6, 32);
            auto kp = generate_key_pair();
            if (!kp) return end_server(id, p, kp.error().message);
            std::uint8_t dh1[32], dh2[32];
            crypto_x25519(dh1, kp->secret.data(), ec.data());
            crypto_x25519(dh2, o_.server_secret->data(), ec.data());
            crypto_wipe(kp->secret.data(), 32);
            derive(p, ec, kp->public_key, static_public_, dh1, dh2, true);
            crypto_wipe(dh1, 32);
            crypto_wipe(dh2, 32);
            std::vector<std::uint8_t> reply(kServerHelloSize);
            reply[0] = kServerHello;
            std::memcpy(reply.data() + 1, kp->public_key.data(), 32);
            std::memcpy(reply.data() + 33, static_public_.data(), 32);
            std::uint8_t nonce[24];
            nonce_for(nonce, ~std::uint64_t{0}, Channel::Reliable); // never used by data packets
            crypto_aead_lock(nullptr, reply.data() + 65, p.k_send.data(), nonce, transcript_hash_.data(), 32, nullptr, 0);
            inner_->send(id, Channel::Reliable, reply);
            p.state = State::AwaitAuth;
            return;
        }
        case State::AwaitServer: {
            if (ev.channel != Channel::Reliable || d.size() != kServerHelloSize || d[0] != kServerHello)
                return end_client(id, p, "the server does not use encryption (or is not a VICTUS server)", out);
            Key es{}, ss{};
            std::memcpy(es.data(), d.data() + 1, 32);
            std::memcpy(ss.data(), d.data() + 33, 32);
            if (o_.expected_server_key && crypto_verify32(o_.expected_server_key->data(), ss.data()) != 0)
                return end_client(id, p, "server key mismatch: expected " + key_to_hex(*o_.expected_server_key) + ", got " + key_to_hex(ss), out);
            std::uint8_t dh1[32], dh2[32];
            crypto_x25519(dh1, p.e_secret.data(), es.data());
            crypto_x25519(dh2, p.e_secret.data(), ss.data());
            derive(p, p.e_public, es, ss, dh1, dh2, false);
            crypto_wipe(dh1, 32);
            crypto_wipe(dh2, 32);
            crypto_wipe(p.e_secret.data(), 32);
            std::uint8_t nonce[24];
            nonce_for(nonce, ~std::uint64_t{0}, Channel::Reliable);
            if (crypto_aead_unlock(nullptr, d.data() + 65, p.k_recv.data(), nonce, transcript_hash_.data(), 32, nullptr, 0) != 0)
                return end_client(id, p, "the server could not prove it holds its key", out);
            p.remote_static = ss;
            p.has_remote_static = true;
            const std::span<const std::uint8_t> pw(reinterpret_cast<const std::uint8_t*>(o_.password.data()),
                                                   std::min(o_.password.size(), kMaxPassword));
            seal(id, p, Channel::Reliable, kAuth, pw);
            p.state = State::AwaitAccept;
            return;
        }
        default: break;
        }
        // Encrypted traffic.
        std::vector<std::uint8_t> plain;
        if (!open(p, ev.channel, d, plain)) {
            ++p.rejected;
            if (p.rejected == 1 || p.rejected % 100 == 0)
                VICTUS_LOG_WARN("net", "discarded {} forged, corrupted or replayed packet(s) from peer {}", p.rejected, id);
            return;
        }
        const std::uint8_t kind = plain[0];
        switch (p.state) {
        case State::AwaitAuth: {
            if (kind != kAuth || ev.channel != Channel::Reliable) return end_server(id, p, "expected authentication");
            bool ok = true;
            if (!o_.password.empty()) {
                // Compare fixed-size hashes in constant time.
                std::uint8_t want[32], got[32];
                crypto_blake2b(want, 32, reinterpret_cast<const std::uint8_t*>(o_.password.data()),
                               std::min(o_.password.size(), kMaxPassword));
                crypto_blake2b(got, 32, plain.data() + 1, plain.size() - 1);
                ok = crypto_verify32(want, got) == 0;
            }
            crypto_wipe(plain.data(), plain.size());
            if (!ok) {
                static const std::string kWrong = "wrong password";
                seal(id, p, Channel::Reliable, kReject,
                     std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(kWrong.data()), kWrong.size()));
                return end_server(id, p, "wrong password");
            }
            seal(id, p, Channel::Reliable, kAccept, {});
            p.state = State::Open;
            out.push_back({TransportEvent::Type::Connected, id, Channel::Reliable, {}, {}});
            return;
        }
        case State::AwaitAccept: {
            if (kind == kAccept) {
                p.state = State::Open;
                out.push_back({TransportEvent::Type::Connected, id, Channel::Reliable, {}, {}});
                return;
            }
            std::string why = kind == kReject ? std::string(plain.begin() + 1, plain.end()) : "unexpected handshake message";
            return end_client(id, p, "server refused the connection: " + why, out);
        }
        case State::Open:
            if (kind != kApp) return;
            plain.erase(plain.begin());
            out.push_back({TransportEvent::Type::Received, id, ev.channel, std::move(plain), {}});
            return;
        default: return;
        }
    }

    std::unique_ptr<Transport> inner_;
    SecureOptions o_;
    Key static_public_{};
    Key transcript_hash_{};
    std::unordered_map<PeerId, Peer> peers_;
    std::vector<TransportEvent> scratch_;
    double now_{0};
};

} // namespace

std::unique_ptr<Transport> with_encryption(std::unique_ptr<Transport> inner, SecureOptions options) {
    return std::make_unique<SecureTransport>(std::move(inner), std::move(options));
}

} // namespace victus::net
