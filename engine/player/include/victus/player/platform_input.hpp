// SPDX-License-Identifier: Apache-2.0
// Adapts the platform input snapshot to the platform-independent gameplay::InputSource used by
// systems and scripts. Shared by the player and the editor's play mode.
#pragma once
#include "victus/gameplay/gameplay.hpp"
#include "victus/platform/platform.hpp"

static_assert(static_cast<int>(victus::platform::GamepadButton::Count) == victus::gameplay::kGamepadButtonCount);
static_assert(static_cast<int>(victus::platform::GamepadAxis::Count) == victus::gameplay::kGamepadAxisCount);
static_assert(victus::platform::InputState::kMaxGamepads == victus::gameplay::kMaxGamepads);

namespace victus::player {

class PlatformInput final : public gameplay::InputSource {
public:
    explicit PlatformInput(const platform::InputState& state) : state_(&state) {}
    // When disabled (e.g. the editor viewport is not focused) every query reports no input.
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    // Mouse coordinates are reported relative to this origin (e.g. the viewport's top-left).
    void set_mouse_origin(math::Vec2 origin) noexcept { origin_ = origin; }

    bool key_down(std::string_view key) const override { return enabled_ && state_->key_down(lookup(key)); }
    bool key_pressed(std::string_view key) const override { return enabled_ && state_->key_pressed(lookup(key)); }
    bool key_released(std::string_view key) const override { return enabled_ && state_->key_released(lookup(key)); }
    bool mouse_down(int button) const override { return enabled_ && valid(button) && state_->mouse_down(button_of(button)); }
    bool mouse_pressed(int button) const override {
        return enabled_ && valid(button) && state_->mouse_pressed(button_of(button));
    }
    math::Vec2 mouse_position() const override { return state_->mouse_position() - origin_; }
    math::Vec2 mouse_delta() const override { return enabled_ ? state_->mouse_delta() : math::Vec2{}; }
    float mouse_wheel() const override { return enabled_ ? state_->wheel() : 0.0f; }
    bool mouse_released(int button) const override {
        return enabled_ && valid(button) && state_->mouse_released(button_of(button));
    }
    bool gamepad_connected(int pad) const override { return enabled_ && pad_ok(pad) && state_->gamepad(idx(pad)).connected; }
    bool gamepad_button(int pad, int button, bool previous = false) const override {
        if (!enabled_ || !pad_ok(pad) || button < 0 || button >= gameplay::kGamepadButtonCount) return false;
        const auto b = static_cast<platform::GamepadButton>(button);
        return previous ? state_->gamepad_button_prev(idx(pad), b)
                        : state_->gamepad(idx(pad)).buttons[static_cast<std::size_t>(button)];
    }
    float gamepad_axis(int pad, int axis, bool previous = false) const override {
        if (!enabled_ || !pad_ok(pad) || axis < 0 || axis >= gameplay::kGamepadAxisCount) return 0;
        const auto a = static_cast<platform::GamepadAxis>(axis);
        return previous ? state_->gamepad_axis_prev(idx(pad), a) : state_->gamepad(idx(pad)).axes[static_cast<std::size_t>(axis)];
    }

    int touch_count() const override { return enabled_ ? static_cast<int>(state_->touches().size()) : 0; }
    gameplay::Touch touch(int i) const override {
        if (i < 0 || i >= touch_count()) return {};
        const auto& t = state_->touches()[static_cast<std::size_t>(i)];
        return {t.id, t.position, t.pressure};
    }

private:
    // Unknown names map to Key::Unknown, which is never down.
    static platform::Key lookup(std::string_view key) noexcept { return platform::key_from_name(key); }
    static bool valid(int b) noexcept { return b >= 0 && b < 3; }
    static bool pad_ok(int p) noexcept { return p >= 0 && p < static_cast<int>(platform::InputState::kMaxGamepads); }
    static std::size_t idx(int p) noexcept { return static_cast<std::size_t>(p); }
    // Script button numbering: 0 left, 1 middle, 2 right (matches platform::MouseButton order).
    static platform::MouseButton button_of(int b) noexcept { return static_cast<platform::MouseButton>(b); }
    const platform::InputState* state_;
    math::Vec2 origin_{};
    bool enabled_{true};
};

} // namespace victus::player
