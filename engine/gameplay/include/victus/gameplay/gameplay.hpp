// SPDX-License-Identifier: Apache-2.0
// Gameplay framework core: a phase-ordered system scheduler that advances a Scene with a fixed
// or variable time step, plus built-in behaviour components usable without scripting.
// Scripting (Luau) and native game modules register their systems through the same scheduler.
#pragma once
#include "victus/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::gameplay {

struct FrameTime {
    double time{0};          // seconds since play started
    float dt{0};             // seconds for this update
    std::uint64_t frame{0};  // update counter
};

enum class Phase : std::uint8_t { PreUpdate, FixedUpdate, Update, PostUpdate };

// Platform-independent input queries for gameplay code and scripts. Keys use the platform key
// names ("A".."Z", "0".."9", "Space", "Enter", "Left", "Right", "Up", "Down", "Escape",
// "LeftShift", "F1".., see platform::key_name). Mouse buttons:
// 0 left, 1 middle, 2 right. Headless runs and servers have no input source.
class InputSource {
public:
    virtual ~InputSource() = default;
    [[nodiscard]] virtual bool key_down(std::string_view key) const = 0;
    [[nodiscard]] virtual bool key_pressed(std::string_view key) const = 0;
    [[nodiscard]] virtual bool key_released(std::string_view key) const = 0;
    [[nodiscard]] virtual bool mouse_down(int button) const = 0;
    [[nodiscard]] virtual bool mouse_pressed(int button) const = 0;
    [[nodiscard]] virtual math::Vec2 mouse_position() const = 0;
    [[nodiscard]] virtual math::Vec2 mouse_delta() const = 0;
};

// Programmatic input for tests, headless runs, bots and replays. `pressed`/`released` edges are
// derived from the previous call to end_frame(), so call it once after every Simulation::tick.
class ManualInput final : public InputSource {
public:
    void set_key(std::string_view key, bool down) {
        auto it = std::find(down_.begin(), down_.end(), key);
        if (down && it == down_.end()) down_.emplace_back(key);
        if (!down && it != down_.end()) down_.erase(it);
    }
    void set_mouse(int button, bool down) {
        if (button >= 0 && button < 8) mouse_[button] = down;
    }
    void set_mouse_position(math::Vec2 p) noexcept { mouse_pos_ = p; }
    void end_frame() {
        prev_ = down_;
        prev_mouse_ = mouse_;
        prev_pos_ = mouse_pos_;
    }
    [[nodiscard]] bool key_down(std::string_view k) const override { return has(down_, k); }
    [[nodiscard]] bool key_pressed(std::string_view k) const override { return has(down_, k) && !has(prev_, k); }
    [[nodiscard]] bool key_released(std::string_view k) const override { return !has(down_, k) && has(prev_, k); }
    [[nodiscard]] bool mouse_down(int b) const override { return b >= 0 && b < 8 && mouse_[b]; }
    [[nodiscard]] bool mouse_pressed(int b) const override { return mouse_down(b) && !prev_mouse_[b]; }
    [[nodiscard]] math::Vec2 mouse_position() const override { return mouse_pos_; }
    [[nodiscard]] math::Vec2 mouse_delta() const override {
        return {mouse_pos_.x - prev_pos_.x, mouse_pos_.y - prev_pos_.y};
    }

private:
    static bool has(const std::vector<std::string>& v, std::string_view k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    }
    std::vector<std::string> down_, prev_;
    std::array<bool, 8> mouse_{}, prev_mouse_{};
    math::Vec2 mouse_pos_{}, prev_pos_{};
};

using SystemFn = std::function<void(scene::Scene&, const FrameTime&)>;

struct SystemInfo {
    std::string name;
    Phase phase{Phase::Update};
    SystemFn fn;
    bool enabled{true};
};

// Runs registered systems over a scene. Fixed-update systems run 0..max_steps times per tick
// at `fixed_dt` (accumulator); other phases run once per tick with the frame delta.
class Simulation {
public:
    explicit Simulation(float fixed_dt = 1.0f / 60.0f) : fixed_dt_(fixed_dt) {}
    void add_system(std::string name, Phase phase, SystemFn fn);
    [[nodiscard]] SystemInfo* find(std::string_view name);
    [[nodiscard]] const std::vector<SystemInfo>& systems() const noexcept { return systems_; }

    // Advances by `dt` seconds (clamped to max_frame_dt). Updates world transforms at the end.
    void tick(scene::Scene& scene, float dt);
    void reset() noexcept;

    [[nodiscard]] const FrameTime& time() const noexcept { return time_; }
    [[nodiscard]] std::uint64_t fixed_steps() const noexcept { return fixed_steps_; }
    [[nodiscard]] float fixed_dt() const noexcept { return fixed_dt_; }
    float max_frame_dt{0.25f};
    std::uint32_t max_fixed_steps{8};
    // Optional input for systems/scripts; owned by the host (player, editor) and may be null.
    const InputSource* input{nullptr};

private:
    void run_phase(scene::Scene& scene, Phase phase, const FrameTime& t);
    std::vector<SystemInfo> systems_;
    float fixed_dt_;
    double accumulator_{0};
    FrameTime time_;
    std::uint64_t fixed_steps_{0};
};

// ---- built-in behaviours -----------------------------------------------------------------------

// Spins the entity around a local axis.
struct RotatorComponent {
    math::Vec3 axis{0, 1, 0};
    float degrees_per_second{45.0f};
};

// Moves the entity sinusoidally around the position it had when play started.
struct OscillatorComponent {
    math::Vec3 amplitude{0, 0.5f, 0};
    float frequency{0.5f}; // Hz
    float phase{0.0f};     // radians
    // Runtime state (not serialized).
    math::Vec3 origin;
    bool has_origin{false};
};

// Registers serializers ("Rotator", "Oscillator"). Idempotent; call once at startup before
// loading scenes so these components are not treated as unknown.
void register_components();
// Adds the built-in behaviour systems to a simulation.
void add_builtin_systems(Simulation& sim);

} // namespace victus::gameplay
