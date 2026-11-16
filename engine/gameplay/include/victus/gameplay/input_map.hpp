// SPDX-License-Identifier: Apache-2.0
// Input actions: named, rebindable game controls ("jump", "move_x") mapped to keys, mouse
// buttons and gamepad buttons/axes. Gameplay code and scripts ask for actions instead of
// physical keys, so one script works with keyboard, mouse and gamepads, and players or
// projects can rebind controls without touching code. Stored in `victus.project` under
// "input"; projects without an "input" section get default_input_map().
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::gameplay {

// One physical control. Text form (as written in project files and accepted by scripts):
//   "Space", "W", "LeftShift"   keyboard key (platform key names)
//   "Mouse0".."Mouse2"           mouse button (0 left, 1 middle, 2 right)
//   "Pad.South", "Pad.Start"     gamepad button (any connected gamepad)
//   "Pad.LeftX+", "Pad.LeftY-"   one half of a gamepad axis (0..1 as the stick moves that way)
//   "Pad.LeftX", "Pad.RightTrigger"   the whole axis, signed
struct InputBinding {
    enum class Kind : std::uint8_t { Key, Mouse, GamepadButton, GamepadAxis };
    Kind kind{Kind::Key};
    std::string key;    // Kind::Key: key name
    int index{0};       // mouse button, gamepad button or axis index
    int direction{0};   // GamepadAxis: +1 / -1 for one half, 0 for the whole signed axis
    friend bool operator==(const InputBinding&, const InputBinding&) = default;
};
[[nodiscard]] Result<InputBinding> parse_binding(std::string_view text);
[[nodiscard]] std::string binding_to_string(const InputBinding& binding);

// A named control. Its value is max(positive bindings) - max(negative bindings), in -1..1; a
// button-like action is "down" when |value| >= 0.5. Axis bindings below `dead_zone` read 0 and
// are rescaled so the value still reaches 1 at full deflection.
struct InputAction {
    std::string name;
    std::vector<InputBinding> positive;
    std::vector<InputBinding> negative;
    float dead_zone{0.2f};
    friend bool operator==(const InputAction&, const InputAction&) = default;
};

class InputMap {
public:
    std::vector<InputAction> actions;

    [[nodiscard]] const InputAction* find(std::string_view name) const noexcept;
    InputAction& add(std::string name, std::vector<std::string_view> positive,
                     std::vector<std::string_view> negative = {}, float dead_zone = 0.2f);

    // Evaluate against an input source (null = no input). `previous` evaluates last frame's
    // state, so pressed/released are edges of `down`.
    [[nodiscard]] static float value(const InputSource* in, const InputAction& action, bool previous = false);
    [[nodiscard]] static float binding_value(const InputSource* in, const InputBinding& binding, float dead_zone,
                                             bool previous = false);
    [[nodiscard]] static bool down(const InputSource* in, const InputAction& a, bool previous = false) {
        const float v = value(in, a, previous);
        return v >= 0.5f || v <= -0.5f;
    }
    [[nodiscard]] static bool pressed(const InputSource* in, const InputAction& a) {
        return down(in, a) && !down(in, a, true);
    }
    [[nodiscard]] static bool released(const InputSource* in, const InputAction& a) {
        return !down(in, a) && down(in, a, true);
    }

    // {"actions": [{"name", "bindings"|"positive", "negative"?, "dead_zone"?}, ...]}
    [[nodiscard]] json::Value to_json() const;
    // Fails on malformed entries, unknown binding syntax and duplicate action names.
    [[nodiscard]] static Result<InputMap> from_json(const json::Value& value);
    // Problems that from_json cannot see, e.g. key names the platform does not know; one
    // message per problem.
    [[nodiscard]] std::vector<std::string> validate(const std::function<bool(std::string_view)>& key_known) const;

    friend bool operator==(const InputMap&, const InputMap&) = default;
};

// The first control that went down this frame, for "press a key to bind" UIs: one of `keys`
// (key names to watch), a mouse button other than Mouse0 (it clicks the UI), a gamepad button,
// or a gamepad axis pushed past half way (a half-axis binding; triggers bind as the whole axis).
[[nodiscard]] std::optional<InputBinding> capture_binding(const InputSource& in, const std::vector<std::string>& keys);

// move_x, move_y, look_x, look_y, jump, fire, interact, sprint, pause, help: WASD/arrows, mouse,
// Space/E/Shift/Escape/H and the matching gamepad sticks and buttons.
[[nodiscard]] InputMap default_input_map();

} // namespace victus::gameplay
