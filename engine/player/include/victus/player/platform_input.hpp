// SPDX-License-Identifier: Apache-2.0
// Adapts the platform input snapshot to the platform-independent gameplay::InputSource used by
// systems and scripts. Shared by the player and the editor's play mode.
#pragma once
#include "victus/gameplay/gameplay.hpp"
#include "victus/platform/platform.hpp"

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

private:
    // Unknown names map to Key::Unknown, which is never down.
    static platform::Key lookup(std::string_view key) noexcept { return platform::key_from_name(key); }
    static bool valid(int b) noexcept { return b >= 0 && b < 3; }
    // Script button numbering: 0 left, 1 middle, 2 right (matches platform::MouseButton order).
    static platform::MouseButton button_of(int b) noexcept { return static_cast<platform::MouseButton>(b); }
    const platform::InputState* state_;
    math::Vec2 origin_{};
    bool enabled_{true};
};

} // namespace victus::player
