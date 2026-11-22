# Netcode research: Unity and Unreal in November 2026 (GAP-37)

Dated **2026-11-22**. VICTUS revision: `main` at `1eebdcf`. This extends section 3 ("Networking" row) of [UNITY_UNREAL_2026_11.md](UNITY_UNREAL_2026_11.md) and the current-state table in [NETWORKING_SPEC.md](../NETWORKING_SPEC.md). Competitor claims come from vendor pages and were not reproduced. Cells marked "Not reviewed" had no vendor page checked for this revision. VICTUS claims come from `engine/net/include/victus/net/net.hpp` and `tests/net/net_tests.cpp`.

## 1. What shipped

### Unity

- **Unity Netcode 7.2.0 (renamed from Netcode for Entities) with the GhostObject API.** Released 2026-11-20 for Unity 6000.7. GameObjects get a `GhostObject`, plus `GhostBehaviour` with a `PredictionUpdate()` method whose `GhostField`s are rolled back, quantised and delta compressed by the same jobified, Burst-compiled serialisation as the ECS path. Netcode for GameObjects 3.2.0 supports hybrid sessions in which a `NetworkObject` and a `GhostObject` share one connection ([GhostObjects release](https://discussions.unity.com/t/prediction-in-gameobject-netcode-ghostobjects-released/1741239)). Until now, client prediction in Unity required ECS.
- **Prediction model.** "Full world prediction": any ghost marked Predicted takes part in rollback and replay, not only the local player. Each client can switch a ghost between interpolated and predicted, for example a "prediction bubble" around the player. Spawns made inside the prediction loop are predicted spawns ([DevLog 4](https://discussions.unity.com/t/prediction-in-gameobject-netcode-devlog-entry-4/1719927), [announcement thread](https://discussions.unity.com/t/prediction-with-gameobjects-coming-to-netcode-for-entities/1698666)).
- **Lag compensation (server rewind).** `NetCodePhysicsConfig` enables it. The server keeps a per-tick collision world in `PhysicsWorldHistorySingleton` and rewinds by the client's `CommandDataInterpolationDelay` to validate hits ([server-side rewind](https://docs.unity3d.com/Packages/com.unity.netcode@6.6/manual/server-rewind.html), [physics](https://docs.unity3d.com/Packages/com.unity.netcode@6.6/manual/physics.html)).
- **Prediction smoothing.** A per-component "smoothing action" spreads corrections over time after each reconciliation ([prediction smoothing](https://docs.unity3d.com/Packages/com.unity.netcode@6.6/manual/prediction-smoothing.html)).
- **Debugging.** Netcode 7.1.0 added a Prediction Tracing Tool that finds which system made a client's predicted state diverge from the server ([Unity Netcode announcements](https://discussions.unity.com/t/unity-netcode-announcements/1736319)). Netcode for GameObjects 3.2.0 adds Unity CLI commands to start and stop sessions, inspect clients and spawned objects, simulate network conditions and sample traffic ([NGO releases](https://github.com/Unity-Technologies/com.unity.netcode.gameobjects/releases)).
- **Distributed authority.** Netcode for GameObjects still offers a topology in which each client is the authority for part of the objects, through Unity's hosted service ([distributed authority](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.12//manual/terms-concepts/authority.html)).

### Unreal

- **Iris** is opt-in, and its 5.9 pages still say "Experimental… use caution when shipping with it". It keeps a quantised copy of all replicated state, shares work between connections, and aims to be push-based: it falls back to polling at `NetUpdateFrequency` for objects without push model. Relevancy becomes Filtering, and priority becomes Prioritization. It replaces the Replication Graph instead of combining with it, and it supports listen servers ([Iris overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine), [Migrate to Iris](https://dev.epicgames.com/documentation/unreal-engine/migrate-to-iris-in-unreal-engine?lang=en-US), [Iris components](https://dev.epicgames.com/documentation/de-de/unreal-engine/components-of-iris-in-unreal-engine)).
- **5.9** adds a Mass Characters plugin with interpolation, visualisation and selective pawn replication ([5.9 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-9-release-notes)).

## 2. VICTUS against these

| Capability | Unity Netcode 7.2 | Unreal (generic and Iris) | VICTUS today |
|---|---|---|---|
| Server authority, client prediction | Yes, full world | Not reviewed for this revision | Yes, local player only (`NetClient` "predicts its own player") |
| Prediction error smoothing | Per-component smoothing actions | Not reviewed | `correction_rate` blend (12/s) and `snap_distance` teleport |
| Interpolation of remote entities | Yes | Yes | `interpolation_delay` (0.1 s) |
| Relevancy and priority | Relevancy, importance scaling | Filtering and Prioritization | `relevancy_radius`, `priority` share of a snapshot budget |
| Delta compression and quantisation | Yes | Yes (quantised state copy) | Delta snapshots that stay exact under loss, quantised snapshots (tests) |
| Loss tolerance for input | Not reviewed | Not reviewed | `input_redundancy` (4 frames per packet) |
| Lag compensation (server rewind) | `PhysicsWorldHistorySingleton` | Not reviewed | **Missing** |
| Predicted spawns | `Instantiate` in the prediction loop | — | **Missing** |
| Prediction bubble (per-ghost mode switch) | Yes | — | **Missing** |
| Divergence tracing for prediction | Prediction Tracing Tool | Not reviewed | **Missing**. Capture and replay divergence reports exist for single-player simulation only |
| Network condition simulation | NGO 3.2 CLI commands | Network Emulation in PIE, on the command line, in the console and in config: in and out lag, loss, jitter, duplication and reordering, with Average and Bad profiles ([Network Emulation](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-network-emulation-in-unreal-engine)) | `Transport` decorator for latency, jitter, loss and bandwidth; Studio Network window |
| Encryption | Not reviewed | Not reviewed | Encrypted sessions (Monocypher), tested over a lossy link |
| Tested off loopback | Shipped games | Shipped games | **No**. All tests are in one process on loopback |

## 3. Recommendations, in order

1. **Lag compensation first.** It is the one gameplay-visible feature that both mainstream paths offer and VICTUS lacks. VICTUS already has fixed-step simulation and Jolt queries, so a ring buffer of per-tick collider transforms on the server plus a rewound ray or shape query (Unity's `PhysicsWorldHistorySingleton` pattern) fits without changing the wire format. It needs an ADR, a `tests/net` case that hits a moving target at 150 ms simulated latency, and a Luau entry point.
2. **Prediction divergence report.** VICTUS already records divergence for capture and replay. Recording the first mismatching component per reconciliation would give a cheap analogue of Unity's tracing tool.
3. **Do not chase full-world prediction yet.** It requires every predicted system to be rollback-safe, which Unity achieves through ECS. VICTUS gameplay runs Luau scripts with side effects. Predicted spawns and a prediction bubble should follow only once script rollback rules exist.
4. **Off-loopback proof (GAP-37).** Two machines over a real WAN, recorded in STATUS.md. Nothing in this table can be called competitive until that exists.

## 4. Re-run conditions

Re-check this document when Unity 6.7 LTS ships (GhostObjects move from 6000.7 beta to LTS), when Epic removes the Experimental label from Iris, or when VICTUS adds lag compensation.
