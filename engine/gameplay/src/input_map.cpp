// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/input_map.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace victus::gameplay {

namespace {

constexpr std::array<const char*, kGamepadButtonCount> kButtonNames{
    "South", "East", "West", "North", "Back", "Guide", "Start", "LeftStick", "RightStick",
    "LeftShoulder", "RightShoulder", "DpadUp", "DpadDown", "DpadLeft", "DpadRight"};
constexpr std::array<const char*, kGamepadAxisCount> kAxisNames{"LeftX", "LeftY", "RightX", "RightY",
                                                                 "LeftTrigger", "RightTrigger"};

Error bad(std::string message) { return make_error(ErrorCode::ParseError, std::move(message)); }

float shape(float v, float dead_zone) {
    const float a = std::fabs(v);
    if (a <= dead_zone) return 0.0f;
    const float dz = std::clamp(dead_zone, 0.0f, 0.95f);
    const float out = std::min(1.0f, (a - dz) / (1.0f - dz));
    return v < 0 ? -out : out;
}

bool key_state(const InputSource& in, std::string_view key, bool previous) {
    if (!previous) return in.key_down(key);
    return in.key_released(key) || (in.key_down(key) && !in.key_pressed(key));
}
bool mouse_state(const InputSource& in, int b, bool previous) {
    if (!previous) return in.mouse_down(b);
    return in.mouse_released(b) || (in.mouse_down(b) && !in.mouse_pressed(b));
}

// Signed contribution of one binding, before the positive/negative split.
float raw(const InputSource& in, const InputBinding& b, float dead_zone, bool previous) {
    switch (b.kind) {
        case InputBinding::Kind::Key: return key_state(in, b.key, previous) ? 1.0f : 0.0f;
        case InputBinding::Kind::Mouse: return mouse_state(in, b.index, previous) ? 1.0f : 0.0f;
        case InputBinding::Kind::GamepadButton:
            for (int p = 0; p < kMaxGamepads; ++p)
                if (in.gamepad_connected(p) && in.gamepad_button(p, b.index, previous)) return 1.0f;
            return 0.0f;
        case InputBinding::Kind::GamepadAxis: {
            // The strongest deflection across connected gamepads wins.
            float best = 0.0f;
            for (int p = 0; p < kMaxGamepads; ++p) {
                if (!in.gamepad_connected(p)) continue;
                float v = in.gamepad_axis(p, b.index, previous);
                if (b.direction != 0) v = std::max(0.0f, v * static_cast<float>(b.direction));
                v = shape(v, dead_zone);
                if (std::fabs(v) > std::fabs(best)) best = v;
            }
            return best;
        }
    }
    return 0.0f;
}

} // namespace

int gamepad_button_index(std::string_view n) noexcept {
    for (int i = 0; i < kGamepadButtonCount; ++i)
        if (n == kButtonNames[static_cast<std::size_t>(i)]) return i;
    if (n == "A") return 0;
    if (n == "B") return 1;
    if (n == "X") return 2;
    if (n == "Y") return 3;
    if (n == "Select") return 4;
    if (n == "Menu") return 6;
    if (n == "LB") return 9;
    if (n == "RB") return 10;
    return -1;
}
int gamepad_axis_index(std::string_view n) noexcept {
    for (int i = 0; i < kGamepadAxisCount; ++i)
        if (n == kAxisNames[static_cast<std::size_t>(i)]) return i;
    if (n == "LT") return 4;
    if (n == "RT") return 5;
    return -1;
}
const char* gamepad_button_name(int i) noexcept {
    return i >= 0 && i < kGamepadButtonCount ? kButtonNames[static_cast<std::size_t>(i)] : "";
}
const char* gamepad_axis_name(int i) noexcept {
    return i >= 0 && i < kGamepadAxisCount ? kAxisNames[static_cast<std::size_t>(i)] : "";
}

Result<InputBinding> parse_binding(std::string_view t) {
    InputBinding b;
    if (t.empty()) return bad("empty input binding");
    if (t.starts_with("Pad.")) {
        std::string_view n = t.substr(4);
        int dir = 0;
        if (!n.empty() && (n.back() == '+' || n.back() == '-')) {
            dir = n.back() == '+' ? 1 : -1;
            n.remove_suffix(1);
        }
        if (const int a = gamepad_axis_index(n); a >= 0) {
            b.kind = InputBinding::Kind::GamepadAxis;
            b.index = a;
            b.direction = dir;
            return b;
        }
        if (const int btn = gamepad_button_index(n); btn >= 0 && dir == 0) {
            b.kind = InputBinding::Kind::GamepadButton;
            b.index = btn;
            return b;
        }
        return bad("unknown gamepad control '" + std::string(t) + "'");
    }
    if (t.starts_with("Mouse")) {
        if (t.size() != 6 || t[5] < '0' || t[5] > '2') return bad("unknown mouse button '" + std::string(t) + "' (Mouse0..Mouse2)");
        b.kind = InputBinding::Kind::Mouse;
        b.index = t[5] - '0';
        return b;
    }
    if (t.find_first_of(" \t\r\n.+") != std::string_view::npos) return bad("invalid key name '" + std::string(t) + "'");
    b.kind = InputBinding::Kind::Key;
    b.key = std::string(t);
    return b;
}

std::string binding_to_string(const InputBinding& b) {
    switch (b.kind) {
        case InputBinding::Kind::Key: return b.key;
        case InputBinding::Kind::Mouse: return "Mouse" + std::to_string(b.index);
        case InputBinding::Kind::GamepadButton: return std::string("Pad.") + gamepad_button_name(b.index);
        case InputBinding::Kind::GamepadAxis:
            return std::string("Pad.") + gamepad_axis_name(b.index) + (b.direction > 0 ? "+" : b.direction < 0 ? "-" : "");
    }
    return {};
}

const InputAction* InputMap::find(std::string_view name) const noexcept {
    for (const auto& a : actions)
        if (a.name == name) return &a;
    return nullptr;
}

InputAction& InputMap::add(std::string name, std::vector<std::string_view> positive,
                           std::vector<std::string_view> negative, float dead_zone) {
    InputAction a;
    a.name = std::move(name);
    a.dead_zone = dead_zone;
    for (auto t : positive)
        if (auto b = parse_binding(t)) a.positive.push_back(std::move(*b));
    for (auto t : negative)
        if (auto b = parse_binding(t)) a.negative.push_back(std::move(*b));
    actions.push_back(std::move(a));
    return actions.back();
}

float InputMap::binding_value(const InputSource* in, const InputBinding& b, float dead_zone, bool previous) {
    return in ? raw(*in, b, dead_zone, previous) : 0.0f;
}

float InputMap::value(const InputSource* in, const InputAction& a, bool previous) {
    if (!in) return 0.0f;
    float pos = 0.0f, neg = 0.0f;
    const auto add = [&](float v) {
        if (v > 0) pos = std::max(pos, v);
        else neg = std::max(neg, -v);
    };
    for (const auto& b : a.positive) add(raw(*in, b, a.dead_zone, previous));
    for (const auto& b : a.negative) add(-raw(*in, b, a.dead_zone, previous));
    return std::clamp(pos - neg, -1.0f, 1.0f);
}

json::Value InputMap::to_json() const {
    json::Value list = json::Value::array();
    for (const auto& a : actions) {
        json::Value o = json::Value::object();
        o.set("name", a.name);
        json::Value pos = json::Value::array();
        for (const auto& b : a.positive) pos.push(binding_to_string(b));
        if (a.negative.empty()) {
            o.set("bindings", std::move(pos));
        } else {
            o.set("positive", std::move(pos));
            json::Value neg = json::Value::array();
            for (const auto& b : a.negative) neg.push(binding_to_string(b));
            o.set("negative", std::move(neg));
        }
        if (a.dead_zone != 0.2f) o.set("dead_zone", a.dead_zone);
        list.push(std::move(o));
    }
    json::Value v = json::Value::object();
    v.set("actions", std::move(list));
    return v;
}

Result<InputMap> InputMap::from_json(const json::Value& v) {
    InputMap m;
    const json::Value* list = v.find("actions");
    if (!v.is_object() || !list || !list->is_array()) return bad("input: expected {\"actions\": [...]}");
    for (const auto& e : list->as_array()) {
        if (!e.is_object() || !e["name"].is_string() || e["name"].as_string().empty())
            return bad("input: every action needs a \"name\"");
        InputAction a;
        a.name = e["name"].as_string();
        if (m.find(a.name)) return bad("input: duplicate action '" + a.name + "'");
        const auto read = [&](std::string_view key, std::vector<InputBinding>& out) -> Status {
            const json::Value* arr = e.find(key);
            if (!arr) return {};
            if (!arr->is_array()) return bad("input: '" + a.name + "." + std::string(key) + "' must be a list");
            for (const auto& s : arr->as_array()) {
                if (!s.is_string()) return bad("input: '" + a.name + "' has a non-string binding");
                auto b = parse_binding(s.as_string());
                if (!b) return bad("input: action '" + a.name + "': " + b.error().message);
                out.push_back(std::move(*b));
            }
            return {};
        };
        if (auto st = read("bindings", a.positive); !st) return st.error();
        if (auto st = read("positive", a.positive); !st) return st.error();
        if (auto st = read("negative", a.negative); !st) return st.error();
        a.dead_zone = std::clamp(e["dead_zone"].as_float(0.2f), 0.0f, 0.95f);
        m.actions.push_back(std::move(a));
    }
    return m;
}

std::vector<std::string> InputMap::validate(const std::function<bool(std::string_view)>& key_known) const {
    std::vector<std::string> problems;
    for (const auto& a : actions) {
        if (a.positive.empty() && a.negative.empty()) problems.push_back("input action '" + a.name + "' has no bindings");
        for (const auto* list : {&a.positive, &a.negative})
            for (const auto& b : *list)
                if (b.kind == InputBinding::Kind::Key && key_known && !key_known(b.key))
                    problems.push_back("input action '" + a.name + "': unknown key '" + b.key + "'");
    }
    return problems;
}

InputMap default_input_map() {
    InputMap m;
    m.add("move_x", {"D", "Right", "Pad.LeftX+", "Pad.DpadRight"}, {"A", "Left", "Pad.LeftX-", "Pad.DpadLeft"});
    m.add("move_y", {"W", "Up", "Pad.LeftY-", "Pad.DpadUp"}, {"S", "Down", "Pad.LeftY+", "Pad.DpadDown"});
    m.add("look_x", {"Pad.RightX+"}, {"Pad.RightX-"});
    m.add("look_y", {"Pad.RightY-"}, {"Pad.RightY+"});
    m.add("jump", {"Space", "Pad.South"});
    m.add("fire", {"Mouse0", "Pad.RightTrigger"});
    m.add("interact", {"E", "Pad.West"});
    m.add("sprint", {"LeftShift", "Pad.LeftStick"});
    m.add("pause", {"Escape", "Pad.Start"});
    m.add("help", {"H", "Pad.Back"});
    m.add("quick_save", {"F5", "Pad.LeftShoulder"});
    m.add("quick_load", {"F9", "Pad.RightShoulder"});
    return m;
}

std::optional<InputBinding> capture_binding(const InputSource& in, const std::vector<std::string>& keys) {
    for (const auto& k : keys)
        if (in.key_pressed(k)) return InputBinding{.kind = InputBinding::Kind::Key, .key = k};
    for (int b = 1; b < 3; ++b)
        if (in.mouse_pressed(b)) return InputBinding{.kind = InputBinding::Kind::Mouse, .index = b};
    for (int pad = 0; pad < kMaxGamepads; ++pad) {
        if (!in.gamepad_connected(pad)) continue;
        for (int b = 0; b < kGamepadButtonCount; ++b)
            if (in.gamepad_button(pad, b) && !in.gamepad_button(pad, b, true))
                return InputBinding{.kind = InputBinding::Kind::GamepadButton, .index = b};
        for (int a = 0; a < kGamepadAxisCount; ++a) {
            const float now = in.gamepad_axis(pad, a), before = in.gamepad_axis(pad, a, true);
            const bool trigger = a >= 4;
            if (std::fabs(now) >= 0.5f && std::fabs(before) < 0.5f)
                return InputBinding{.kind = InputBinding::Kind::GamepadAxis, .index = a,
                                    .direction = trigger ? 0 : (now > 0 ? 1 : -1)};
        }
    }
    return std::nullopt;
}

} // namespace victus::gameplay
