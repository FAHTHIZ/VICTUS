// SPDX-License-Identifier: Apache-2.0
// VICTUS platform layer: process lifecycle, windows, events and raw input devices.
// Backed by SDL3 internally; SDL types never appear in VICTUS public headers.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace victus::platform {

enum class Key : std::uint16_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Escape, Enter, Tab, Backspace, Space, Delete, Insert, Home, End, PageUp, PageDown,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt, LeftSuper, RightSuper,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Minus, Equals, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Grave, Comma, Period, Slash,
    Count
};
[[nodiscard]] const char* key_name(Key key) noexcept;
[[nodiscard]] Key key_from_name(std::string_view name) noexcept;

enum class MouseButton : std::uint8_t { Left, Middle, Right, X1, X2, Count };

enum class GamepadButton : std::uint8_t {
    South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, Count
};
enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

struct GamepadState {
    bool connected{false};
    std::uint32_t id{0};
    std::string name;
    std::array<bool, static_cast<std::size_t>(GamepadButton::Count)> buttons{};
    std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> axes{};
};

struct TouchPoint {
    std::uint64_t id{0};
    math::Vec2 position; // normalized [0,1]
    float pressure{0};
};

// Snapshot of input device state for the current frame, plus per-frame edges.
class InputState {
public:
    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
    static constexpr std::size_t kMouseCount = static_cast<std::size_t>(MouseButton::Count);
    static constexpr std::size_t kMaxGamepads = 4;

    [[nodiscard]] bool key_down(Key k) const noexcept { return keys_[idx(k)]; }
    [[nodiscard]] bool key_pressed(Key k) const noexcept { return keys_[idx(k)] && !prev_keys_[idx(k)]; }
    [[nodiscard]] bool key_released(Key k) const noexcept { return !keys_[idx(k)] && prev_keys_[idx(k)]; }
    [[nodiscard]] bool mouse_down(MouseButton b) const noexcept { return mouse_[static_cast<std::size_t>(b)]; }
    [[nodiscard]] bool mouse_pressed(MouseButton b) const noexcept {
        return mouse_[static_cast<std::size_t>(b)] && !prev_mouse_[static_cast<std::size_t>(b)];
    }
    [[nodiscard]] math::Vec2 mouse_position() const noexcept { return mouse_pos_; }
    [[nodiscard]] math::Vec2 mouse_delta() const noexcept { return mouse_delta_; }
    [[nodiscard]] float wheel() const noexcept { return wheel_; }
    [[nodiscard]] const std::string& text_input() const noexcept { return text_; }
    [[nodiscard]] const GamepadState& gamepad(std::size_t i) const noexcept { return pads_[i < kMaxGamepads ? i : 0]; }
    [[nodiscard]] const std::vector<TouchPoint>& touches() const noexcept { return touches_; }

    // Mutation API used by the platform backend, input playback and tests.
    void begin_frame() noexcept;
    void set_key(Key k, bool down) noexcept { keys_[idx(k)] = down; }
    void set_mouse_button(MouseButton b, bool down) noexcept { mouse_[static_cast<std::size_t>(b)] = down; }
    void set_mouse_position(math::Vec2 p) noexcept {
        mouse_delta_ += p - mouse_pos_;
        mouse_pos_ = p;
    }
    void add_mouse_motion(math::Vec2 rel) noexcept { mouse_delta_ += rel; }
    void add_wheel(float w) noexcept { wheel_ += w; }
    void add_text(const std::string& t) { text_ += t; }
    GamepadState& gamepad_mut(std::size_t i) noexcept { return pads_[i < kMaxGamepads ? i : 0]; }
    std::vector<TouchPoint>& touches_mut() noexcept { return touches_; }

private:
    static std::size_t idx(Key k) noexcept { return static_cast<std::size_t>(k) < kKeyCount ? static_cast<std::size_t>(k) : 0; }
    std::array<bool, kKeyCount> keys_{}, prev_keys_{};
    std::array<bool, kMouseCount> mouse_{}, prev_mouse_{};
    math::Vec2 mouse_pos_{}, mouse_delta_{};
    float wheel_{0};
    std::string text_;
    std::array<GamepadState, kMaxGamepads> pads_{};
    std::vector<TouchPoint> touches_;
};

enum class EventType : std::uint8_t {
    Quit, WindowResized, WindowClose, WindowFocusGained, WindowFocusLost, WindowMinimized, WindowRestored,
    FileDropped, AppWillEnterBackground, AppDidEnterForeground, AppLowMemory, AppTerminating
};
struct Event {
    EventType type{};
    std::uint32_t window_id{0};
    int width{0}, height{0};
    std::string path;
};

struct WindowDesc {
    std::string title{"VICTUS"};
    int width{1280};
    int height{720};
    bool resizable{true};
    bool maximized{false};
    bool fullscreen{false};
    bool hidden{false};
    bool high_dpi{true};
};

class Window {
public:
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    [[nodiscard]] std::uint32_t id() const noexcept;
    [[nodiscard]] math::Vec2 size() const noexcept;        // logical points
    [[nodiscard]] math::Vec2 pixel_size() const noexcept;  // framebuffer pixels
    [[nodiscard]] float display_scale() const noexcept;
    void set_title(const std::string& title);
    void set_relative_mouse(bool enabled);
    void set_text_input(bool enabled);
    [[nodiscard]] void* native() const noexcept { return handle_; } // SDL_Window*, for engine backends only

private:
    friend class Platform;
    explicit Window(void* handle) : handle_(handle) {}
    void* handle_;
};

struct PlatformDesc {
    bool video{true};     // false = headless (servers, CI, simulation hosts): no window/GPU/audio
    bool audio{false};
    bool gamepad{true};
    std::string app_name{"VICTUS"};
};

// Owns platform subsystem lifetime. Exactly one instance per process.
class Platform {
public:
    [[nodiscard]] static Result<std::unique_ptr<Platform>> create(const PlatformDesc& desc);
    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    [[nodiscard]] Result<std::unique_ptr<Window>> create_window(const WindowDesc& desc);
    // Pumps OS events, updates input state, returns lifecycle events. Call once per frame.
    std::vector<Event> poll_events();
    [[nodiscard]] InputState& input() noexcept { return input_; }
    [[nodiscard]] bool headless() const noexcept { return !desc_.video; }
    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }
    void request_quit() noexcept { quit_ = true; }
    [[nodiscard]] const char* video_driver() const noexcept;
    [[nodiscard]] static double seconds() noexcept;
    [[nodiscard]] std::string clipboard() const;
    void set_clipboard(const std::string& text);

private:
    explicit Platform(const PlatformDesc& desc) : desc_(desc) {}
    void handle_gamepad_added(std::uint32_t id);
    PlatformDesc desc_;
    InputState input_;
    bool quit_{false};
    std::vector<void*> gamepads_;
};

} // namespace victus::platform
