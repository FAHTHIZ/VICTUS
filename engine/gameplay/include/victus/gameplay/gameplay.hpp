// SPDX-License-Identifier: Apache-2.0
// Gameplay framework core: a phase-ordered system scheduler that advances a Scene with a fixed
// or variable time step, plus built-in behaviour components usable without scripting.
// Scripting (Luau) and native game modules register their systems through the same scheduler.
#pragma once
#include "victus/scene/scene.hpp"

#include <functional>
#include <string>
#include <vector>

namespace victus::gameplay {

struct FrameTime {
    double time{0};          // seconds since play started
    float dt{0};             // seconds for this update
    std::uint64_t frame{0};  // update counter
};

enum class Phase : std::uint8_t { PreUpdate, FixedUpdate, Update, PostUpdate };

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
    float max_frame_dt{0.25f};
    std::uint32_t max_fixed_steps{8};

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
