// SPDX-License-Identifier: Apache-2.0
// Deterministic regression captures: a play session's per-frame input and frame times, the
// random seed its scripts used, and state digests taken while it was recorded. Replaying the
// capture (headless or in the player) must reproduce the same digests; a mismatch pinpoints the
// first frame where gameplay diverged.
#pragma once

#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace victus::gameplay {

inline constexpr std::uint32_t kCaptureFormatVersion = 1;

// Every key name InputSource implementations understand (see platform::key_name).
[[nodiscard]] const std::vector<std::string_view>& key_names();

// Everything an InputSource reports for one frame.
struct InputState {
    std::vector<std::string> keys; // held keys, in key_names() order
    std::uint8_t mouse{0};         // bit per mouse button 0..7
    math::Vec2 mouse_position;
    math::Vec2 mouse_delta;
    struct Pad {
        bool connected{false};
        std::uint32_t buttons{0}; // bit per gamepad button
        std::array<float, kGamepadAxisCount> axes{};
        bool operator==(const Pad&) const = default;
    };
    std::array<Pad, kMaxGamepads> pads{};
    bool operator==(const InputState&) const = default;
};

[[nodiscard]] InputState sample_input(const InputSource& in);

// Plays InputStates back: set() before a tick, end_frame() after it (edges come from the
// previous state, exactly as they did for the recorded source).
class ReplayInput final : public InputSource {
public:
    void set(const InputState& s) { cur_ = s; }
    void end_frame() { prev_ = cur_; }
    [[nodiscard]] const InputState& state() const noexcept { return cur_; }

    [[nodiscard]] bool key_down(std::string_view k) const override { return has(cur_, k); }
    [[nodiscard]] bool key_pressed(std::string_view k) const override { return has(cur_, k) && !has(prev_, k); }
    [[nodiscard]] bool key_released(std::string_view k) const override { return !has(cur_, k) && has(prev_, k); }
    [[nodiscard]] bool mouse_down(int b) const override { return bit(cur_, b); }
    [[nodiscard]] bool mouse_pressed(int b) const override { return bit(cur_, b) && !bit(prev_, b); }
    [[nodiscard]] bool mouse_released(int b) const override { return !bit(cur_, b) && bit(prev_, b); }
    [[nodiscard]] math::Vec2 mouse_position() const override { return cur_.mouse_position; }
    [[nodiscard]] math::Vec2 mouse_delta() const override { return cur_.mouse_delta; }
    [[nodiscard]] bool gamepad_connected(int pad) const override {
        return pad >= 0 && pad < kMaxGamepads && cur_.pads[static_cast<std::size_t>(pad)].connected;
    }
    [[nodiscard]] bool gamepad_button(int pad, int button, bool previous = false) const override {
        if (pad < 0 || pad >= kMaxGamepads || button < 0 || button >= kGamepadButtonCount) return false;
        return ((previous ? prev_ : cur_).pads[static_cast<std::size_t>(pad)].buttons >> button) & 1u;
    }
    [[nodiscard]] float gamepad_axis(int pad, int axis, bool previous = false) const override {
        if (pad < 0 || pad >= kMaxGamepads || axis < 0 || axis >= kGamepadAxisCount) return 0;
        return (previous ? prev_ : cur_).pads[static_cast<std::size_t>(pad)].axes[static_cast<std::size_t>(axis)];
    }

private:
    static bool has(const InputState& s, std::string_view k);
    static bool bit(const InputState& s, int b) { return b >= 0 && b < 8 && ((s.mouse >> b) & 1); }
    InputState cur_, prev_;
};

// Order-independent hash of the scene's gameplay state: every entity's name path and transform
// (rounded to 0.1 mm / 1e-4) plus the entity count. Equal scenes give equal digests across runs;
// runtime UUIDs and creation order do not matter.
[[nodiscard]] std::string state_digest(const scene::Scene& scene);

struct Capture {
    std::string project;   // informational: project name
    std::string scene;     // project-relative scene the session started in
    std::uint64_t seed{0}; // script random seed (math.random)
    std::vector<float> frame_dt;
    std::vector<InputState> frames;
    struct Digest {
        std::uint64_t frame{0}; // taken after this many ticks
        std::string value;
    };
    std::vector<Digest> digests;
    std::uint32_t digest_interval{60}; // frames between digests while recording

    // Records one tick's input; call before the tick.
    void add_frame(const InputState& input, float dt);
    // Call after each tick while recording: stores a digest every digest_interval frames.
    void after_tick(const scene::Scene& scene);
    // Stores the final digest (call once when recording stops).
    void finish(const scene::Scene& scene);

    [[nodiscard]] Status save(const std::filesystem::path& path) const;
    [[nodiscard]] static Result<Capture> load(const std::filesystem::path& path);
    [[nodiscard]] static Result<Capture> parse(std::string_view text);
    [[nodiscard]] std::string to_text() const;
};

// Replays a capture tick by tick and checks its digests.
class CaptureReplay {
public:
    explicit CaptureReplay(const Capture& capture) : capture_(&capture) {}
    [[nodiscard]] bool done() const noexcept { return frame_ >= capture_->frames.size(); }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }
    // Input for the next tick and its frame time.
    [[nodiscard]] float begin_tick();
    // After the tick: compares a digest recorded for this frame. False on the first mismatch.
    bool end_tick(const scene::Scene& scene);
    [[nodiscard]] ReplayInput& input() noexcept { return input_; }
    [[nodiscard]] std::uint64_t digests_checked() const noexcept { return checked_; }
    [[nodiscard]] bool diverged() const noexcept { return !mismatch_.empty(); }
    // "frame 120: expected <digest>, got <digest>" for the first divergence.
    [[nodiscard]] const std::string& mismatch() const noexcept { return mismatch_; }

private:
    const Capture* capture_;
    ReplayInput input_;
    std::uint64_t frame_{0};
    std::size_t next_digest_{0};
    std::uint64_t checked_{0};
    std::string mismatch_;
};

} // namespace victus::gameplay
