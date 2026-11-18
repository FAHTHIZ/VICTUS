// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/input_map.hpp"

#include "doctest.h"

using namespace victus;
using gameplay::InputBinding;
using gameplay::InputMap;
using gameplay::ManualInput;

TEST_CASE("input map: binding syntax round-trips and rejects nonsense") {
    for (const char* t : {"Space", "W", "LeftShift", "Mouse0", "Mouse2", "Pad.South", "Pad.Start", "Pad.DpadLeft",
                          "Pad.LeftX+", "Pad.LeftY-", "Pad.RightTrigger", "Pad.RightX"}) {
        auto b = gameplay::parse_binding(t);
        REQUIRE_MESSAGE(b.ok(), t);
        CHECK(gameplay::binding_to_string(*b) == t);
    }
    auto a = gameplay::parse_binding("Pad.A");
    REQUIRE(a.ok());
    CHECK(a->kind == InputBinding::Kind::GamepadButton);
    CHECK(gameplay::binding_to_string(*a) == "Pad.South"); // aliases normalize
    CHECK(gameplay::parse_binding("Pad.RT")->index == gameplay::gamepad_axis_index("RightTrigger"));
    auto half = gameplay::parse_binding("Pad.LeftX-");
    CHECK(half->kind == InputBinding::Kind::GamepadAxis);
    CHECK(half->direction == -1);
    for (const char* t : {"", "Pad.", "Pad.Nope", "Pad.South+", "Mouse9", "Left Shift", "A+B"})
        CHECK_MESSAGE(!gameplay::parse_binding(t).ok(), t);
}

TEST_CASE("input map: keys, mouse and gamepads drive the same actions") {
    const InputMap m = gameplay::default_input_map();
    const auto* move_x = m.find("move_x");
    const auto* jump = m.find("jump");
    const auto* fire = m.find("fire");
    REQUIRE(move_x);
    REQUIRE(jump);
    REQUIRE(fire);
    CHECK(m.find("nope") == nullptr);

    ManualInput in;
    CHECK(InputMap::value(nullptr, *move_x) == 0.0f);
    CHECK(InputMap::value(&in, *move_x) == 0.0f);
    in.set_key("D", true);
    CHECK(InputMap::value(&in, *move_x) == 1.0f);
    in.set_key("A", true); // opposite directions cancel
    CHECK(InputMap::value(&in, *move_x) == 0.0f);
    in.set_key("A", false);
    in.set_key("D", false);

    // Stick: dead zone, rescaling, the stronger of key and stick, either direction.
    const int lx = gameplay::gamepad_axis_index("LeftX");
    in.set_gamepad_axis(0, lx, 0.15f);
    CHECK(InputMap::value(&in, *move_x) == 0.0f);
    in.set_gamepad_axis(0, lx, 0.6f);
    CHECK(InputMap::value(&in, *move_x) == doctest::Approx(0.5f));
    CHECK(InputMap::down(&in, *move_x));
    in.set_gamepad_axis(0, lx, -1.0f);
    CHECK(InputMap::value(&in, *move_x) == doctest::Approx(-1.0f));
    in.set_gamepad_axis(0, lx, 0.0f);
    // Other gamepads count too: per direction the strongest deflection wins, and opposite
    // directions on different pads cancel like opposite keys.
    in.set_gamepad_axis(2, lx, -0.9f);
    CHECK(InputMap::value(&in, *move_x) == doctest::Approx(-0.875f));
    in.set_gamepad_axis(1, lx, 0.3f);
    CHECK(InputMap::value(&in, *move_x) == doctest::Approx(-0.75f));
    in.set_gamepad_axis(1, lx, 0.0f);
    in.set_gamepad_axis(2, lx, 0.0f);
    // A disconnected pad's stale state is ignored.
    in.set_gamepad_axis(3, lx, 1.0f);
    in.set_gamepad_connected(3, false);
    CHECK(InputMap::value(&in, *move_x) == 0.0f);

    // Edges: pressed on the first frame only, released after letting go.
    in.end_frame();
    in.set_gamepad_button(0, gameplay::gamepad_button_index("South"), true);
    CHECK(InputMap::pressed(&in, *jump));
    in.end_frame();
    CHECK(InputMap::down(&in, *jump));
    CHECK_FALSE(InputMap::pressed(&in, *jump));
    in.set_gamepad_button(0, gameplay::gamepad_button_index("South"), false);
    CHECK(InputMap::released(&in, *jump));
    in.end_frame();
    CHECK_FALSE(InputMap::released(&in, *jump));
    in.set_key("Space", true);
    CHECK(InputMap::pressed(&in, *jump));
    in.end_frame();
    in.set_key("Space", false);
    CHECK(InputMap::released(&in, *jump));
    in.end_frame();

    // Triggers and mouse buttons.
    in.set_gamepad_axis(0, gameplay::gamepad_axis_index("RightTrigger"), 0.8f);
    CHECK(InputMap::value(&in, *fire) == doctest::Approx(0.75f));
    in.set_gamepad_axis(0, gameplay::gamepad_axis_index("RightTrigger"), 0.0f);
    in.end_frame();
    in.set_mouse(0, true);
    CHECK(InputMap::pressed(&in, *fire));
    in.end_frame();
    in.set_mouse(0, false);
    CHECK(InputMap::released(&in, *fire));
}

TEST_CASE("input map: project JSON round-trip, validation and errors") {
    InputMap m;
    m.add("steer", {"Right", "Pad.LeftX"}, {"Left"}, 0.1f);
    m.add("boost", {"LeftShift", "Pad.East"});
    const json::Value j = m.to_json();
    auto back = InputMap::from_json(j);
    REQUIRE(back.ok());
    CHECK(*back == m);
    CHECK(InputMap::from_json(gameplay::default_input_map().to_json()).value() == gameplay::default_input_map());

    const auto parse = [](const char* text) {
        auto v = json::parse(text);
        REQUIRE(v.ok());
        return InputMap::from_json(*v);
    };
    CHECK_FALSE(parse(R"({"actions": [{"name": "a", "bindings": ["Pad.Nope"]}]})").ok());
    CHECK_FALSE(parse(R"({"actions": [{"name": "a"}, {"name": "a"}]})").ok());
    CHECK_FALSE(parse(R"({"actions": [{"bindings": ["Space"]}]})").ok());
    CHECK_FALSE(parse(R"({"actions": {"name": "a"}})").ok());
    CHECK_FALSE(parse(R"({"actions": [{"name": "a", "bindings": [3]}]})").ok());
    auto ok = parse(R"({"actions": [{"name": "a", "bindings": ["Qq"]}, {"name": "b"}]})");
    REQUIRE(ok.ok());
    const auto problems = ok->validate([](std::string_view k) { return k != "Qq"; });
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].find("unknown key 'Qq'") != std::string::npos);
    CHECK(problems[1].find("no bindings") != std::string::npos);
}

TEST_CASE("input map: capture_binding reports the first new control") {
    using namespace victus::gameplay;
    ManualInput in;
    const std::vector<std::string> keys{"Space", "J"};
    CHECK_FALSE(capture_binding(in, keys).has_value());
    in.set_mouse(0, true); // the click on the UI itself is ignored
    CHECK_FALSE(capture_binding(in, keys).has_value());
    in.set_mouse(2, true);
    CHECK(binding_to_string(*capture_binding(in, keys)) == "Mouse2");
    in.end_frame();
    CHECK_FALSE(capture_binding(in, keys).has_value()); // held, not new
    in.set_key("J", true);
    CHECK(binding_to_string(*capture_binding(in, keys)) == "J");
    in.end_frame();
    in.set_gamepad_button(1, gamepad_button_index("North"), true);
    CHECK(binding_to_string(*capture_binding(in, keys)) == "Pad.North");
    in.end_frame();
    in.set_gamepad_axis(0, gamepad_axis_index("LeftY"), 0.3f); // not past half way
    CHECK_FALSE(capture_binding(in, keys).has_value());
    in.set_gamepad_axis(0, gamepad_axis_index("LeftY"), 0.8f);
    CHECK(binding_to_string(*capture_binding(in, keys)) == "Pad.LeftY+");
    in.end_frame();
    in.set_gamepad_axis(0, gamepad_axis_index("RightTrigger"), 1.0f);
    CHECK(binding_to_string(*capture_binding(in, keys)) == "Pad.RightTrigger");
}

namespace {
// An input source whose key went down and up again within the frame (a tap at a low frame rate).
class TapInput final : public gameplay::InputSource {
public:
    bool key_down(std::string_view) const override { return false; }
    bool key_pressed(std::string_view k) const override { return k == key; }
    bool key_released(std::string_view k) const override { return k == key; }
    bool mouse_down(int) const override { return false; }
    bool mouse_pressed(int b) const override { return b == button; }
    bool mouse_released(int b) const override { return b == button; }
    math::Vec2 mouse_position() const override { return {}; }
    math::Vec2 mouse_delta() const override { return {}; }
    std::string_view key;
    int button{-1};
};
} // namespace

TEST_CASE("input map: an action tapped within one frame still fires") {
    const InputMap m = gameplay::default_input_map();
    const auto* jump = m.find("jump");
    const auto* fire = m.find("fire");
    REQUIRE(jump);
    REQUIRE(fire);
    TapInput in;
    in.key = "Space";
    CHECK(InputMap::pressed(&in, *jump));
    CHECK(InputMap::down(&in, *jump));
    CHECK_FALSE(InputMap::down(&in, *jump, true));
    in.key = {};
    in.button = 0;
    CHECK(InputMap::pressed(&in, *fire));
}
