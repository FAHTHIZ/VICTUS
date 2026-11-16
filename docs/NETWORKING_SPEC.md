# VICTUS Networking Specification

## Goals
Provide reusable foundations for local, peer-connected and dedicated-server games without forcing a proprietary online service. Networking is modular and game rules remain owned by the project.

## Architecture boundaries
Separate transport, connection/session lifecycle, serialization, replication, prediction, interpolation, relevancy, authority, matchmaking/service adapters, diagnostics and game-specific rules. Keep network-facing APIs behind VICTUS interfaces.

## Authority and replication
- Default to server-authoritative shared state.
- Define ownership and permissions for every replicated entity/property/event.
- Support reliable and unreliable message classes with explicit ordering semantics.
- Replicate snapshots/deltas with quantization/compression only after correctness tests.
- Interest management/relevancy must scale with world size and player count.
- Validate all client input and payloads; clients never authoritatively commit shared state.

## Responsive simulation
Support client-side prediction for eligible local actions, server reconciliation, snapshot interpolation and latency-tolerant movement. Design rollback only where gameplay determinism and profiling justify it. Document nondeterministic systems and reconciliation limits.

## Dedicated servers
Provide headless server builds that run authoritative gameplay/simulation without renderer, editor or audio device initialization. Include configuration, structured logs, health diagnostics, graceful shutdown, persistence hooks and deployment packaging.

## Test and debug
Provide deterministic network simulation for latency, jitter, packet loss, reordering and bandwidth caps. Add replication inspectors, per-entity ownership views, bandwidth counters, server tick metrics, replay/capture and reconnect tests. Test two or more clients against a local dedicated server in CI where feasible.

## Secure sessions
Implemented in Slice 26 (`engine/net/src/secure.cpp`, ADR-0028). `with_encryption()` wraps any transport. Handshake on the reliable channel: the client sends `0xA1 "VSEC" version e_c`; the server answers `0xA2 e_s s_s tag`, where both sides compute `h = BLAKE2b("VICTUS-SECURE-1" e_c e_s s_s)` and `key_c2s/key_s2c = BLAKE2b(key h, DH(e_c,e_s) || DH(e_c,s_s) || "c2s"/"s2c")`, and `tag` is an empty XChaCha20-Poly1305 message under `key_s2c` with `h` as associated data. The client checks a pinned key if configured, verifies the tag and sends its password (possibly empty) encrypted; the server answers accept, or reject with a reason and disconnects. Data packets are `0xA3 counter:u64 ciphertext mac:16`; the nonce is the counter and channel, the header and channel are associated data, and the first plaintext byte is the message kind. Reliable counters must increase; unreliable counters pass a 64-packet window. Servers keep a long-term secret key (`<project>/.victus/server.key` for `victus serve`) whose public half players can pin with `--server-key`.

## Online services
Expose replaceable interfaces for identity, lobbies, matchmaking, friends, leaderboards, voice and platform services. Do not make any one vendor or hosted service mandatory. Document privacy, security, costs and platform restrictions before integrating providers.

## Acceptance
A sample multiplayer game must run with at least two clients and a dedicated authoritative server, show replicated state and responsive movement under simulated network conditions, and pass validation/security tests. Do not mark multiplayer complete based on local loopback alone.
