// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/gameplay.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>

namespace victus::gameplay {

using json::Value;

void Simulation::add_system(std::string name, Phase phase, SystemFn fn) {
    systems_.push_back({std::move(name), phase, std::move(fn), true});
}

SystemInfo* Simulation::find(std::string_view name) {
    for (auto& s : systems_)
        if (s.name == name) return &s;
    return nullptr;
}

void Simulation::reset() noexcept {
    accumulator_ = 0;
    time_ = {};
    fixed_steps_ = 0;
}

void Simulation::run_phase(scene::Scene& scene, Phase phase, const FrameTime& t) {
    for (auto& s : systems_) {
        if (s.enabled && s.phase == phase && s.fn) s.fn(scene, t);
    }
}

void Simulation::tick(scene::Scene& scene, float dt) {
    dt = std::clamp(dt, 0.0f, max_frame_dt);
    FrameTime t = time_;
    t.dt = dt;
    run_phase(scene, Phase::PreUpdate, t);
    accumulator_ += dt;
    std::uint32_t steps = 0;
    while (accumulator_ >= fixed_dt_ && steps < max_fixed_steps) {
        FrameTime ft = t;
        ft.dt = fixed_dt_;
        ft.time = static_cast<double>(fixed_steps_) * fixed_dt_;
        run_phase(scene, Phase::FixedUpdate, ft);
        accumulator_ -= fixed_dt_;
        ++fixed_steps_;
        ++steps;
    }
    if (steps == max_fixed_steps) accumulator_ = std::min(accumulator_, static_cast<double>(fixed_dt_));
    run_phase(scene, Phase::Update, t);
    run_phase(scene, Phase::PostUpdate, t);
    scene.update_transforms();
    time_.time += dt;
    time_.dt = dt;
    ++time_.frame;
}

void register_components() {
    auto& r = scene::SerializerRegistry::instance();
    r.add<RotatorComponent>(
        "Rotator",
        [](const RotatorComponent& c) {
            Value v = Value::object();
            v.set("axis", scene::to_json(c.axis));
            v.set("degrees_per_second", c.degrees_per_second);
            return v;
        },
        [](RotatorComponent& c, const Value& v) -> Status {
            if (const Value* a = v.find("axis")) scene::from_json(*a, c.axis);
            c.degrees_per_second = v["degrees_per_second"].as_float(c.degrees_per_second);
            return {};
        });
    r.add<OscillatorComponent>(
        "Oscillator",
        [](const OscillatorComponent& c) {
            Value v = Value::object();
            v.set("amplitude", scene::to_json(c.amplitude));
            v.set("frequency", c.frequency);
            v.set("phase", c.phase);
            return v;
        },
        [](OscillatorComponent& c, const Value& v) -> Status {
            if (const Value* a = v.find("amplitude")) scene::from_json(*a, c.amplitude);
            c.frequency = v["frequency"].as_float(c.frequency);
            c.phase = v["phase"].as_float(c.phase);
            return {};
        });
}

void add_builtin_systems(Simulation& sim) {
    sim.add_system("builtin.rotator", Phase::Update, [](scene::Scene& s, const FrameTime& t) {
        VICTUS_PROFILE_SCOPE("gameplay.rotator");
        s.world().each<RotatorComponent, scene::Transform>([&](const RotatorComponent& r, scene::Transform& tr) {
            if (math::length(r.axis) <= 0) return;
            const math::Quat dq = math::Quat::axis_angle(r.axis, math::radians(r.degrees_per_second) * t.dt);
            tr.rotation = math::normalize(tr.rotation * dq);
        });
    });
    sim.add_system("builtin.oscillator", Phase::Update, [](scene::Scene& s, const FrameTime& t) {
        VICTUS_PROFILE_SCOPE("gameplay.oscillator");
        const double now = t.time + t.dt;
        s.world().each<OscillatorComponent, scene::Transform>([&](OscillatorComponent& o, scene::Transform& tr) {
            if (!o.has_origin) {
                o.origin = tr.position;
                o.has_origin = true;
            }
            const float k = static_cast<float>(std::sin(2.0 * 3.14159265358979 * o.frequency * now + o.phase));
            tr.position = o.origin + o.amplitude * k;
        });
    });
}

} // namespace victus::gameplay
