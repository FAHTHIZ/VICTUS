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
Implemented in Slice 30 (ADR-0032): `NetServer::replication()`/`client_diagnostics()` and `NetClient::replication()` expose per-entity owner, updates, bytes, variable traffic, last update tick and relevance, and per-client RTT, traffic, acks, input queue and violations; the editor shows them in its Network window.

## Listen servers
Implemented in Slice 27 (ADR-0029). `ServerOptions::host_player` spawns a server-owned (`kServerId`) copy of each player template for the host at the template position; input routing falls back to the server's own input for server-owned entities, so the host's character runs the same script code as offline. Rosters and `clients()` list the host first as id 0; `NetServer::local_player()` returns its character. Remote clients see it as an ordinary interpolated player. The host has no prediction, interpolation or corrections; the session ends when the host leaves (no migration yet).

## Reconnect
Implemented in Slice 28 (ADR-0030, protocol 4). Welcome: `u16 id, u32 tick, u64 token`; hello ends with `u64 token` (0 = new player). A connection that drops without a goodbye parks the client's character for `reconnect_grace` seconds (idle, out of the roster, counted toward capacity, `client_dropped`); a hello with its token restores id, name and character (`client_rejoined`), retiring any live connection still holding that token. Expiry despawns (`client_left`). Clients retry for `reconnect_window` seconds; on the rejoin welcome they reset baselines, input history and clock sync, re-confirm known entities from the re-sent spawns and prune the rest when the roster arrives.

## Secure sessions
Implemented in Slice 26 (`engine/net/src/secure.cpp`, ADR-0028). `with_encryption()` wraps any transport. Handshake on the reliable channel: the client sends `0xA1 "VSEC" version e_c`; the server answers `0xA2 e_s s_s tag`, where both sides compute `h = BLAKE2b("VICTUS-SECURE-1" e_c e_s s_s)` and `key_c2s/key_s2c = BLAKE2b(key h, DH(e_c,e_s) || DH(e_c,s_s) || "c2s"/"s2c")`, and `tag` is an empty XChaCha20-Poly1305 message under `key_s2c` with `h` as associated data. The client checks a pinned key if configured, verifies the tag and sends its password (possibly empty) encrypted; the server answers accept, or reject with a reason and disconnects. Data packets are `0xA3 counter:u64 ciphertext mac:16`; the nonce is the counter and channel, the header and channel are associated data, and the first plaintext byte is the message kind. Reliable counters must increase; unreliable counters pass a 64-packet window. Servers keep a long-term secret key (`<project>/.victus/server.key` for `victus serve`) whose public half players can pin with `--server-key`.

## Online services
Expose replaceable interfaces for identity, lobbies, matchmaking, friends, leaderboards, voice and platform services. Do not make any one vendor or hosted service mandatory. Document privacy, security, costs and platform restrictions before integrating providers.

## Acceptance
A sample multiplayer game must run with at least two clients and a dedicated authoritative server, show replicated state and responsive movement under simulated network conditions, and pass validation/security tests. Do not mark multiplayer complete based on local loopback alone.

## Current state (2026-11-22)

`engine/net` has had no feature changes since the editor replication inspector (ADR-0032) and in-editor clients (ADR-0033). Later commits touched only warnings and pack signing. Checked against `engine/net/include/victus/net/net.hpp`:

| Spec area | On main | Missing |
|---|---|---|
| Authority and replication | Server-authoritative ECS replication over ENet with reliable and unreliable channels (ADR-0020), delta snapshots against acknowledged baselines (ADR-0025), per-template ownership, a bandwidth budget with relevancy radius and priority, replicated script variables, and replicated gameplay components (Attributes, Inventory, Tags; ADR-0258). | Snapshots already carry quantised transforms and input axes (`session.cpp`). Scripts exchange named, ordered messages (`on_net_message`). Missing: property-level permissions, typed RPCs bound to entities, and interest groups beyond distance. |
| Responsive simulation | Client prediction of the local player, reconciliation with error blending (`correction_rate`), interpolation of remote entities (`interpolation_delay`), and input redundancy. | Lag-compensated hit detection (server rewind) and rollback. Prediction covers the player hierarchy only, not physics objects or abilities. Unity's Netcode for GameObjects added GhostObjects prediction in 2026 (see [the Unity/Unreal audit](audits/UNITY_UNREAL_2026_11.md)). |
| Dedicated servers | `victus serve` runs the authoritative session with a long-term key (ADR-0028). | Structured health endpoint, persistence hooks and deployment packaging. Never run off loopback. |
| Test and debug | Network condition decorator (latency, jitter, loss, bandwidth; parsed from `latency=80,jitter=10,...`), replication inspector and per-client diagnostics (ADR-0032), and play-in-editor clients over loopback UDP (ADR-0033). | Tests run on loopback only. No packet capture or replay of network sessions. |
| Sessions | Encryption (ADR-0028), listen server (ADR-0029) and reconnect with tokens (ADR-0030). | NAT traversal, relays, matchmaking, lobbies and host migration. |

This is the "Networking (ENet) — Experimental" row in [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).

Comparison with Unity Netcode 7.2 and Unreal Iris, and the recommended order of the missing pieces: [audits/NETCODE_RESEARCH_2026_11.md](audits/NETCODE_RESEARCH_2026_11.md).
