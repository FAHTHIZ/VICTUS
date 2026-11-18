// SPDX-License-Identifier: Apache-2.0
// VICTUS platform layer: process lifecycle, windows, events and raw input devices.
// Backed by SDL3 internally; SDL types never appear in VICTUS public headers.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"

#include <array>
#include <cstdint>
#include <functional>
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
    // Edges are latched as events arrive, so a key or button that goes down and back up between two
    // begin_frame() calls (a quick tap at a low frame rate) still reports pressed and released for that
    // frame even though key_down() is already false again.
    [[nodiscard]] bool key_pressed(Key k) const noexcept { return key_pressed_[idx(k)]; }
    [[nodiscard]] bool key_released(Key k) const noexcept { return key_released_[idx(k)]; }
    // Whether the key was held when this frame began (before this frame's events).
    [[nodiscard]] bool key_was_down(Key k) const noexcept { return prev_keys_[idx(k)]; }
    [[nodiscard]] bool mouse_down(MouseButton b) const noexcept { return mouse_[midx(b)]; }
    [[nodiscard]] bool mouse_pressed(MouseButton b) const noexcept { return mouse_pressed_[midx(b)]; }
    [[nodiscard]] bool mouse_released(MouseButton b) const noexcept { return mouse_released_[midx(b)]; }
    [[nodiscard]] bool mouse_was_down(MouseButton b) const noexcept { return prev_mouse_[midx(b)]; }
    [[nodiscard]] math::Vec2 mouse_position() const noexcept { return mouse_pos_; }
    [[nodiscard]] math::Vec2 mouse_delta() const noexcept { return mouse_delta_; }
    [[nodiscard]] float wheel() const noexcept { return wheel_; }
    [[nodiscard]] const std::string& text_input() const noexcept { return text_; }
    [[nodiscard]] const GamepadState& gamepad(std::size_t i) const noexcept { return pads_[i < kMaxGamepads ? i : 0]; }
    // Gamepad buttons/axes as they were before this frame's events (for press/release edges).
    [[nodiscard]] bool gamepad_button_prev(std::size_t pad, GamepadButton b) const noexcept {
        return prev_pad_buttons_[pad < kMaxGamepads ? pad : 0][static_cast<std::size_t>(b)];
    }
    [[nodiscard]] float gamepad_axis_prev(std::size_t pad, GamepadAxis a) const noexcept {
        return prev_pad_axes_[pad < kMaxGamepads ? pad : 0][static_cast<std::size_t>(a)];
    }
    [[nodiscard]] const std::vector<TouchPoint>& touches() const noexcept { return touches_; }

    // Mutation API used by the platform backend, input playback and tests.
    void begin_frame() noexcept;
    void set_key(Key k, bool down) noexcept {
        if (k == Key::Unknown) return; // unmapped keys are never reported as held
        const std::size_t i = idx(k);
        if (down && !keys_[i]) key_pressed_[i] = true;
        if (!down && keys_[i]) key_released_[i] = true;
        keys_[i] = down;
    }
    void set_mouse_button(MouseButton b, bool down) noexcept {
        const std::size_t i = midx(b);
        if (down && !mouse_[i]) mouse_pressed_[i] = true;
        if (!down && mouse_[i]) mouse_released_[i] = true;
        mouse_[i] = down;
    }
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
    static std::size_t midx(MouseButton b) noexcept {
        return static_cast<std::size_t>(b) < kMouseCount ? static_cast<std::size_t>(b) : 0;
    }
    std::array<bool, kKeyCount> keys_{}, prev_keys_{}, key_pressed_{}, key_released_{};
    std::array<bool, kMouseCount> mouse_{}, prev_mouse_{}, mouse_pressed_{}, mouse_released_{};
    math::Vec2 mouse_pos_{}, mouse_delta_{};
    float wheel_{0};
    std::string text_;
    std::array<GamepadState, kMaxGamepads> pads_{};
    std::array<std::array<bool, static_cast<std::size_t>(GamepadButton::Count)>, kMaxGamepads> prev_pad_buttons_{};
    std::array<std::array<float, static_cast<std::size_t>(GamepadAxis::Count)>, kMaxGamepads> prev_pad_axes_{};
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
    // Withdraws a quit request (e.g. the user cancelled closing a window with unsaved work).
    void cancel_quit() noexcept { quit_ = false; }
    // Native folder picker. Asynchronous: `done` runs on the calling thread inside a later
    // poll_events() with the chosen folder, or an empty string when cancelled/unavailable.
    void show_folder_dialog(Window* parent, const std::string& default_location,
                            std::function<void(std::string)> done);
    // Native "open files" picker allowing several files. Asynchronous like show_folder_dialog:
    // `done` runs inside a later poll_events() with the chosen paths (empty when cancelled).
    void show_open_files_dialog(Window* parent, const std::string& default_location,
                                std::function<void(std::vector<std::string>)> done);
    [[nodiscard]] const char* video_driver() const noexcept;
    [[nodiscard]] static double seconds() noexcept;
    // Writable per-user folder for saves, caches and unpacked content (ends with a separator). On Android this is
    // the app's internal storage; on desktop the OS preference folder for `org`/`app`. Empty on failure.
    [[nodiscard]] static std::string user_data_dir(const std::string& org, const std::string& app);
    // Folder the player can reach from outside the app (ends with a separator): on Android the app's external
    // files folder (Android/data/<package>/files, readable over USB), for logs a player can send. Empty elsewhere
    // or on failure.
    [[nodiscard]] static std::string external_data_dir();
    // Modal error box for failures before (or instead of) any rendering, so a game does not just vanish. Works
    // without a Platform instance or window. False when no dialog could be shown (e.g. headless). Skipped
    // (false) under the offscreen/dummy video drivers or when VICTUS_NO_DIALOGS is set, so tests never block.
    static bool show_error_box(const std::string& title, const std::string& message);
    // Reads a file shipped with the application. Relative paths resolve against the APK `assets/` on Android and
    // against the executable's folder elsewhere; absolute paths are read as-is.
    [[nodiscard]] static Result<std::vector<std::uint8_t>> read_bundled(const std::string& path);
    // Reads at most `max_bytes` from the start of a bundled file and reports its total size in `total_size`.
    [[nodiscard]] static Result<std::vector<std::uint8_t>> read_bundled_prefix(const std::string& path, std::size_t max_bytes,
                                                                               std::uint64_t* total_size = nullptr);
    // Streams a bundled file to `dest` in 1 MiB chunks (no full copy in memory), writing `dest`.tmp first and renaming it
    // into place so an interrupted copy never leaves a truncated file. Creates missing parent folders. Returns the size.
    [[nodiscard]] static Result<std::uint64_t> copy_bundled(const std::string& path, const std::string& dest);
    // True on phones and tablets (touch-first input, small screens, battery and thermal limits).
    [[nodiscard]] static bool is_mobile() noexcept;
    [[nodiscard]] std::string clipboard() const;
    void set_clipboard(const std::string& text);
    // Receives every raw OS event (an `SDL_Event*`) before VICTUS processes it. Intended for
    // engine-level integrations such as the UI layer; game code should use `input()`.
    using NativeEventHook = std::function<void(const void* native_event)>;
    void set_native_event_hook(NativeEventHook hook) { native_hook_ = std::move(hook); }

private:
    explicit Platform(const PlatformDesc& desc) : desc_(desc) {}
    void handle_gamepad_added(std::uint32_t id);
    PlatformDesc desc_;
    InputState input_;
    bool quit_{false};
    std::vector<void*> gamepads_;
    NativeEventHook native_hook_;
    struct DialogState;
    std::shared_ptr<DialogState> dialogs_;
};

} // namespace victus::platform
