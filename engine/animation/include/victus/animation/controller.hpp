// SPDX-License-Identifier: Apache-2.0
// Animation controllers: `.vanim` state machines whose states play a clip or a 1D/2D blend
// space of clips, switched by transitions with parameter conditions and cross-fades
// (ADR-0068, docs/ANIMATION.md).
//
//     { "version": 1,
//       "parameters": [ { "name": "speed", "type": "float" },
//                       { "name": "grounded", "type": "bool", "default": true },
//                       { "name": "jump", "type": "trigger" } ],
//       "default_state": "Locomotion",
//       "states": [
//         { "name": "Locomotion", "blend1d": { "parameter": "speed",
//             "points": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1.5 }, { "clip": "Run", "at": 4 } ] } },
//         { "name": "Jump", "clip": "Jump", "loop": false } ],
//       "transitions": [
//         { "from": "Locomotion", "to": "Jump", "duration": 0.1, "conditions": [ { "parameter": "jump" } ] },
//         { "from": "Jump", "to": "Locomotion", "duration": 0.2, "exit_time": 0.8,
//           "conditions": [ { "parameter": "grounded", "op": "true" } ] } ] }
//
// The asset only names clips; they resolve against the Animator's model when it plays, so one
// controller can drive every model that has clips of those names.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::animation {

inline constexpr std::string_view kControllerExtension = ".vanim";
inline constexpr std::uint32_t kControllerVersion = 1;

enum class ParamType : std::uint8_t {
    Float,
    Int,     // stored as a float, rounded when set
    Bool,    // 0 or 1
    Trigger, // set until a transition that tests it fires (or it is reset)
};
[[nodiscard]] std::string_view param_type_name(ParamType t) noexcept;

struct ControllerParam {
    std::string name;
    ParamType type{ParamType::Float};
    float default_value{0};
    friend bool operator==(const ControllerParam&, const ControllerParam&) = default;
};

enum class MotionType : std::uint8_t { Clip, Blend1D, Blend2D };

// A clip placed in a blend space: `at.x` is the 1D threshold; 2D spaces use x and y.
struct BlendPoint {
    std::string clip;
    math::Vec2 at{};
    float speed{1.0f}; // per-clip playback rate inside the space
    friend bool operator==(const BlendPoint& a, const BlendPoint& b) {
        return a.clip == b.clip && a.at.x == b.at.x && a.at.y == b.at.y && a.speed == b.speed;
    }
};

struct ControllerState {
    std::string name;
    MotionType motion{MotionType::Clip};
    std::string clip;                // MotionType::Clip
    std::string param_x, param_y;    // blend parameters (Blend1D uses param_x)
    std::vector<BlendPoint> points;  // Blend1D: sorted by at.x
    float speed{1.0f};               // playback rate of the state
    std::string speed_parameter;     // optional float parameter multiplied into the rate
    bool loop{true};
    friend bool operator==(const ControllerState&, const ControllerState&) = default;
};

enum class ConditionOp : std::uint8_t {
    True,    // bool parameter set (default for bool parameters)
    False,   // bool parameter clear
    Trigger, // trigger set (default and only operator for triggers); consumed when the transition fires
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
    Equal,
    NotEqual,
};
[[nodiscard]] std::string_view condition_op_name(ConditionOp op) noexcept;

struct Condition {
    std::string parameter;
    ConditionOp op{ConditionOp::True};
    float value{0};
    std::int32_t index{-1}; // parameter index (resolved by parse)
    friend bool operator==(const Condition&, const Condition&) = default;
};

struct ControllerTransition {
    std::int32_t from{-1}; // state index, -1 = any state ("*")
    std::int32_t to{0};
    float duration{0.2f};  // cross-fade seconds (0 = cut)
    // Normalized time of the source state (1 = one full cycle) before the transition may fire;
    // < 0 = no exit time. On looping states an exit time below 1 is checked every cycle, on the
    // frame that point of the cycle is crossed; otherwise it holds once reached (conditions may
    // become true later).
    float exit_time{-1.0f};
    float offset{0.0f};    // normalized start time in the destination state
    bool allow_self{false}; // any-state transitions only: may restart the state it is already in
    std::vector<Condition> conditions; // all must hold
    friend bool operator==(const ControllerTransition&, const ControllerTransition&) = default;
};

class Controller {
  public:
    static constexpr std::size_t kMaxStates = 256;
    static constexpr std::size_t kMaxParameters = 256;
    static constexpr std::size_t kMaxTransitions = 4096;
    static constexpr std::size_t kMaxPointsPerState = 64;
    static constexpr std::size_t kMaxConditions = 32;

    // Validates names, references and values. Errors name the offending element.
    [[nodiscard]] static Result<std::shared_ptr<const Controller>> parse(std::string_view text, std::string name = {});
    [[nodiscard]] static Result<std::shared_ptr<const Controller>> from_json(const json::Value& doc, std::string name = {});
    // Deterministic document; from_json(to_json()) round-trips.
    [[nodiscard]] json::Value to_json() const;

    [[nodiscard]] std::int32_t find_state(std::string_view name) const noexcept;
    [[nodiscard]] std::int32_t find_parameter(std::string_view name) const noexcept;
    // Every distinct clip name the controller references, in first-use order.
    [[nodiscard]] std::vector<std::string> clip_names() const;

    std::string name; // asset path (diagnostics)
    std::vector<ControllerParam> parameters;
    std::vector<ControllerState> states;
    std::vector<ControllerTransition> transitions;
    std::int32_t default_state{0};
};

// 1D blend weights: linear between the two thresholds around x, clamped at the ends.
// `thresholds` must be sorted ascending; `weights` receives one value per threshold (sum 1).
void blend_weights_1d(std::span<const float> thresholds, float x, std::span<float> weights) noexcept;
// 2D Cartesian gradient-band weights (Johansen 2009, sec. 6.3; Unity's "Freeform Cartesian"):
// exact (weight 1) at each point, smooth in between, sum 1. Coincident points split evenly.
void blend_weights_2d(std::span<const math::Vec2> points, math::Vec2 p, std::span<float> weights) noexcept;

} // namespace victus::animation
