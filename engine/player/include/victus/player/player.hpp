// SPDX-License-Identifier: Apache-2.0
// The VICTUS game player: opens a project, loads its startup scene and runs it in a window with
// the gameplay simulation. Used by `victus run`, the standalone player executable shipped with
// exported games, and automated smoke tests (frame limit + screenshot).
#pragma once
#include "victus/core/profiler.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/render/render_path.hpp"
#include "victus/render/device_class.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace victus::player {

// Script random seed of fixed-frame runs (`victus run --frames`, `victus simulate`) that do not
// choose one, so the same project, scene and frame count always produce the same game.
inline constexpr std::uint64_t kFixedRunSeed = 1;

struct PlayerOptions {
    fs::Path project;            // project folder or victus.project
    std::string scene;           // project-relative; empty = startup scene
    int width{0}, height{0};     // 0 = project settings
    std::uint64_t max_frames{0}; // 0 = until closed; >0 also makes time deterministic (1/60 s)
    std::string screenshot;      // PNG written after the last frame (requires max_frames)
    // Frame sequence: with max_frames, every capture_every-th frame is written to
    // capture_dir/frame_NNNNN.png (frame number from 1) for `victus compare` (ADR-0102).
    fs::Path capture_dir;
    std::uint32_t capture_every{1};
    bool fullscreen{false};
    // Interactive runs load textures and models on background jobs and upload them under a
    // per-frame budget (V.A.U.L.T., ADR-0087); fixed-frame, recorded and replayed runs always
    // load synchronously so captures and screenshots stay deterministic.
    bool async_assets{true};
    // GPU device losses survived by recreating the device and every GPU object (ADR-0157). After
    // that many, or when recreation fails, the player stops with the loss (ErrorCode::DeviceError).
    std::uint32_t device_recoveries{3};
    bool hot_reload{false};      // reload changed scripts, textures, models and audio while running (development)
    std::string quality;         // low|medium|high|ultra|auto; empty = project rendering.quality
    // Packed R11G11B10F HDR scene colour; unset = compact_hdr_by_default(tier, mobile).
    std::optional<bool> compact_hdr;
    // Explicit FP16 BRDF maths where supported; unset = half_precision_by_default(tier, mobile).
    std::optional<bool> half_precision;
    // Overrides of the tier's 3D resolution scale (0.25..2) and TAA, after the project settings;
    // unset = tier/project values. For upscaler comparisons such as `victus run --render-scale 0.5`.
    std::optional<float> render_scale;
    std::optional<bool> taa;
    std::optional<bool> reactive_mask; // TAA reactive mask for particles, water and rain (ADR-0143)
    std::optional<std::uint32_t> msaa_samples; // MSAA of the opaque pass: 1, 2, 4 or 8 (ADR-0152)
    std::string connect;         // host[:port] of a `victus serve` server; empty = play offline
    std::string player_name{"player"}; // name shown to other players when connected
    std::string net_conditions;  // link emulation for testing, e.g. "latency=80,loss=0.05"
    std::string server_key;      // expected server public key (hex); empty = accept and report it
    // Expected Ed25519 signer of game.vpak (hex, ADR-0154). Empty = the `signing_public_key` of
    // the export.json next to the game folder, if any. When a signer is expected an unsigned or
    // differently signed pack is refused; a signed pack's signature is always checked.
    std::string pack_key;
    std::string net_password;    // sent to servers started with --password (over the encrypted channel)
    bool insecure{false};        // connect without encryption (to servers started with --insecure)
    // Listen server: host a session on UDP `host_port` and play in it (others join with
    // connect). Encrypted with `host_key` (created if missing; empty = `<project>/.victus/
    // server.key`, or next to the user's saves for exported games) unless `insecure`;
    // `net_password` protects it.
    bool host{false};
    std::uint16_t host_port{0};  // 0 = net::kDefaultPort
    std::uint16_t max_clients{8};
    fs::Path host_key;
    // Save games folder; empty = `<user data>/saves/<game>` for exported games and
    // `<project>/.victus/saves` when running a project.
    fs::Path saves;
    // Regression captures (offline only): record this session's input, frame times, script
    // random seed and state digests to a .vcapture file, or replay one (keyboard ignored; the run
    // ends with the capture and reports the first frame whose state digest differs).
    fs::Path record;
    // Script random seed (math.random). 0 = kFixedRunSeed when max_frames is set (fixed-step
    // runs repeat exactly) and Luau's time-based seed in interactive sessions. Recordings store
    // the seed they used; replays take it from the capture.
    std::uint64_t random_seed{0};
    // Profiling (ADR-0061): write a frame-time/scope summary (JSON) and/or a Chrome/Perfetto
    // trace of the session (the first 600 frames) when it ends.
    fs::Path profile;
    fs::Path trace;
    fs::Path replay;
};

struct PlayerReport {
    std::uint64_t frames{0};
    std::uint64_t frames_captured{0}; // PNGs written to PlayerOptions::capture_dir
    // Rendering path of the last 3D frame (ADR-0103); no features when the last frame was 2D.
    render::RenderPathReport render_path;
    double seconds{0};
    std::string backend;
    std::uint64_t script_errors{0};
    bool packaged{false};        // ran from an exported game.vpak
    std::size_t plugins_loaded{0}; // native plugins initialized from <project>/plugins (ADR-0078)
    std::string quality;         // quality tier used
    std::string hdr_format;      // HDR scene colour format used: "R11G11B10F" or "RGBA16F"
    std::string device_class;    // with quality "auto": why that tier was picked (ADR-0127)
    std::string net_status;      // last multiplayer status when --connect was used
    std::uint64_t assets_streamed{0}; // textures/models uploaded by the background loader
    std::size_t assets_reloaded{0}; // textures/models/audio hot-reloaded while running
    std::uint64_t digests_checked{0}; // replay: state digests compared
    std::string replay_mismatch;    // replay: first divergence ("frame N: expected X, got Y")
    std::uint64_t scene_loads{0};   // scene transitions scripts made (scene.load/reload)
    std::string final_scene;        // the scene running when the player stopped
    std::uint32_t world_cells_peak{0}; // world streaming: most cells active at once (ADR-0090)
    std::uint64_t world_cell_loads{0}; // world streaming: cells activated
    std::uint32_t device_recoveries{0}; // GPU device losses recovered from (ADR-0157)
    profiler::FrameReport profile;  // with PlayerOptions::profile or trace
};

// Packed HDR by default on phones and on the Low and Medium tiers, where framebuffer bandwidth
// limits the frame rate (ADR-0126).
[[nodiscard]] constexpr bool compact_hdr_by_default(render::QualityTier tier, bool mobile) noexcept {
    return mobile || tier == render::QualityTier::Low || tier == render::QualityTier::Medium;
}
// FP16 shading by default where ALU and register pressure limit the frame: phones and the Low and
// Medium tiers (ADR-0137). Only takes effect on devices with shaderFloat16.
[[nodiscard]] constexpr bool half_precision_by_default(render::QualityTier tier, bool mobile) noexcept {
    return mobile || tier == render::QualityTier::Low || tier == render::QualityTier::Medium;
}
[[nodiscard]] Result<PlayerReport> run(const PlayerOptions& options);

} // namespace victus::player
