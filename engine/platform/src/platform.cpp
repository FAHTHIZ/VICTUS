// SPDX-License-Identifier: Apache-2.0
#include "victus/platform/platform.hpp"

#include "victus/core/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>

#include <cstring>
#include <format>

namespace victus::platform {

namespace {
constexpr const char* kKeyNames[] = {
    "Unknown", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Escape", "Enter", "Tab",
    "Backspace", "Space", "Delete", "Insert", "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down",
    "LeftShift", "RightShift", "LeftCtrl", "RightCtrl", "LeftAlt", "RightAlt", "LeftSuper", "RightSuper", "F1", "F2",
    "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Minus", "Equals", "LeftBracket", "RightBracket",
    "Backslash", "Semicolon", "Apostrophe", "Grave", "Comma", "Period", "Slash"};
static_assert(std::size(kKeyNames) == static_cast<std::size_t>(Key::Count));

Key map_scancode(SDL_Scancode sc) noexcept {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return static_cast<Key>(static_cast<int>(Key::A) + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return static_cast<Key>(static_cast<int>(Key::Num1) + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return static_cast<Key>(static_cast<int>(Key::F1) + (sc - SDL_SCANCODE_F1));
    switch (sc) {
    case SDL_SCANCODE_0: return Key::Num0;
    case SDL_SCANCODE_ESCAPE: return Key::Escape;
    case SDL_SCANCODE_RETURN: return Key::Enter;
    case SDL_SCANCODE_KP_ENTER: return Key::Enter;
    case SDL_SCANCODE_TAB: return Key::Tab;
    case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
    case SDL_SCANCODE_SPACE: return Key::Space;
    case SDL_SCANCODE_DELETE: return Key::Delete;
    case SDL_SCANCODE_INSERT: return Key::Insert;
    case SDL_SCANCODE_HOME: return Key::Home;
    case SDL_SCANCODE_END: return Key::End;
    case SDL_SCANCODE_PAGEUP: return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN: return Key::PageDown;
    case SDL_SCANCODE_LEFT: return Key::Left;
    case SDL_SCANCODE_RIGHT: return Key::Right;
    case SDL_SCANCODE_UP: return Key::Up;
    case SDL_SCANCODE_DOWN: return Key::Down;
    case SDL_SCANCODE_LSHIFT: return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT: return Key::RightShift;
    case SDL_SCANCODE_LCTRL: return Key::LeftCtrl;
    case SDL_SCANCODE_RCTRL: return Key::RightCtrl;
    case SDL_SCANCODE_LALT: return Key::LeftAlt;
    case SDL_SCANCODE_RALT: return Key::RightAlt;
    case SDL_SCANCODE_LGUI: return Key::LeftSuper;
    case SDL_SCANCODE_RGUI: return Key::RightSuper;
    case SDL_SCANCODE_MINUS: return Key::Minus;
    case SDL_SCANCODE_EQUALS: return Key::Equals;
    case SDL_SCANCODE_LEFTBRACKET: return Key::LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return Key::RightBracket;
    case SDL_SCANCODE_BACKSLASH: return Key::Backslash;
    case SDL_SCANCODE_SEMICOLON: return Key::Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return Key::Apostrophe;
    case SDL_SCANCODE_GRAVE: return Key::Grave;
    case SDL_SCANCODE_COMMA: return Key::Comma;
    case SDL_SCANCODE_PERIOD: return Key::Period;
    case SDL_SCANCODE_SLASH: return Key::Slash;
    default: return Key::Unknown;
    }
}

int map_mouse(Uint8 b) noexcept {
    switch (b) {
    case SDL_BUTTON_LEFT: return static_cast<int>(MouseButton::Left);
    case SDL_BUTTON_MIDDLE: return static_cast<int>(MouseButton::Middle);
    case SDL_BUTTON_RIGHT: return static_cast<int>(MouseButton::Right);
    case SDL_BUTTON_X1: return static_cast<int>(MouseButton::X1);
    case SDL_BUTTON_X2: return static_cast<int>(MouseButton::X2);
    default: return -1;
    }
}

int map_pad_button(Uint8 b) noexcept {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return 0;
    case SDL_GAMEPAD_BUTTON_EAST: return 1;
    case SDL_GAMEPAD_BUTTON_WEST: return 2;
    case SDL_GAMEPAD_BUTTON_NORTH: return 3;
    case SDL_GAMEPAD_BUTTON_BACK: return 4;
    case SDL_GAMEPAD_BUTTON_GUIDE: return 5;
    case SDL_GAMEPAD_BUTTON_START: return 6;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 7;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 8;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 9;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 10;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return 11;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 12;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 13;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 14;
    default: return -1;
    }
}
} // namespace

const char* key_name(Key key) noexcept {
    const auto i = static_cast<std::size_t>(key);
    return i < std::size(kKeyNames) ? kKeyNames[i] : "Unknown";
}

Key key_from_name(std::string_view name) noexcept {
    for (std::size_t i = 0; i < std::size(kKeyNames); ++i)
        if (name == kKeyNames[i]) return static_cast<Key>(i);
    return Key::Unknown;
}

void InputState::begin_frame() noexcept {
    prev_keys_ = keys_;
    key_pressed_ = {};
    key_released_ = {};
    for (std::size_t i = 0; i < kMaxGamepads; ++i) {
        prev_pad_buttons_[i] = pads_[i].buttons;
        prev_pad_axes_[i] = pads_[i].axes;
    }
    prev_mouse_ = mouse_;
    mouse_pressed_ = {};
    mouse_released_ = {};
    mouse_delta_ = {};
    wheel_ = 0;
    text_.clear();
}

Window::~Window() {
    if (handle_) SDL_DestroyWindow(static_cast<SDL_Window*>(handle_));
}
std::uint32_t Window::id() const noexcept { return SDL_GetWindowID(static_cast<SDL_Window*>(handle_)); }
math::Vec2 Window::size() const noexcept {
    int w = 0, h = 0;
    SDL_GetWindowSize(static_cast<SDL_Window*>(handle_), &w, &h);
    return {static_cast<float>(w), static_cast<float>(h)};
}
math::Vec2 Window::pixel_size() const noexcept {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(handle_), &w, &h);
    return {static_cast<float>(w), static_cast<float>(h)};
}
float Window::display_scale() const noexcept {
    const float s = SDL_GetWindowDisplayScale(static_cast<SDL_Window*>(handle_));
    return s > 0 ? s : 1.0f;
}
void Window::set_title(const std::string& title) { SDL_SetWindowTitle(static_cast<SDL_Window*>(handle_), title.c_str()); }
void Window::set_relative_mouse(bool enabled) { SDL_SetWindowRelativeMouseMode(static_cast<SDL_Window*>(handle_), enabled); }
void Window::set_text_input(bool enabled) {
    if (enabled) SDL_StartTextInput(static_cast<SDL_Window*>(handle_));
    else SDL_StopTextInput(static_cast<SDL_Window*>(handle_));
}

Result<std::unique_ptr<Platform>> Platform::create(const PlatformDesc& desc) {
    SDL_SetHint(SDL_HINT_APP_NAME, desc.app_name.c_str());
    SDL_InitFlags flags = SDL_INIT_EVENTS;
    if (desc.video) flags |= SDL_INIT_VIDEO;
    if (desc.audio) flags |= SDL_INIT_AUDIO;
    if (desc.gamepad && desc.video) flags |= SDL_INIT_GAMEPAD;
    if (!SDL_Init(flags)) return make_error(ErrorCode::DeviceError, std::format("SDL_Init failed: {}", SDL_GetError()));
    auto p = std::unique_ptr<Platform>(new Platform(desc));
    VICTUS_LOG_INFO("platform", "initialized (video={}, driver={})", desc.video, p->video_driver());
    return p;
}

Platform::~Platform() {
    for (void* g : gamepads_)
        if (g) SDL_CloseGamepad(static_cast<SDL_Gamepad*>(g));
    SDL_Quit();
}

const char* Platform::video_driver() const noexcept {
    const char* d = desc_.video ? SDL_GetCurrentVideoDriver() : nullptr;
    return d ? d : "none";
}

double Platform::seconds() noexcept { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

std::string Platform::user_data_dir(const std::string& org, const std::string& app) {
#if defined(__ANDROID__)
    (void)org;
    (void)app;
    const char* internal = SDL_GetAndroidInternalStoragePath();
    return internal ? std::string(internal) + "/" : std::string();
#else
    char* p = SDL_GetPrefPath(org.c_str(), app.c_str());
    if (!p) return {};
    std::string out(p);
    SDL_free(p);
    return out;
#endif
}

namespace {
// Relative bundled paths resolve against the APK assets on Android (SDL's file IO does that) and the executable folder elsewhere.
std::string bundled_path(const std::string& path) {
    std::string full = path;
#if !defined(__ANDROID__)
    const bool absolute = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!absolute)
        if (const char* base = SDL_GetBasePath()) full = std::string(base) + path;
#endif
    return full;
}
struct IoCloser {
    void operator()(SDL_IOStream* io) const noexcept { SDL_CloseIO(io); }
};
using IoPtr = std::unique_ptr<SDL_IOStream, IoCloser>;
} // namespace

Result<std::vector<std::uint8_t>> Platform::read_bundled_prefix(const std::string& path, std::size_t max_bytes,
                                                                std::uint64_t* total_size) {
    IoPtr io(SDL_IOFromFile(bundled_path(path).c_str(), "rb"));
    if (!io) return make_error(ErrorCode::NotFound, std::format("bundled file '{}': {}", path, SDL_GetError()));
    const Sint64 size = SDL_GetIOSize(io.get());
    if (size < 0) return make_error(ErrorCode::IoError, std::format("bundled file '{}': {}", path, SDL_GetError()));
    if (total_size) *total_size = static_cast<std::uint64_t>(size);
    std::vector<std::uint8_t> out(std::min<std::uint64_t>(max_bytes, static_cast<std::uint64_t>(size)));
    std::size_t got = 0;
    while (got < out.size()) {
        const std::size_t n = SDL_ReadIO(io.get(), out.data() + got, out.size() - got);
        if (n == 0) break;
        got += n;
    }
    out.resize(got);
    return out;
}

Result<std::uint64_t> Platform::copy_bundled(const std::string& path, const std::string& dest) {
    IoPtr in(SDL_IOFromFile(bundled_path(path).c_str(), "rb"));
    if (!in) return make_error(ErrorCode::NotFound, std::format("bundled file '{}': {}", path, SDL_GetError()));
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::path(dest);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    const std::string tmp = dest + ".tmp";
    std::uint64_t total = 0;
    {
        IoPtr out(SDL_IOFromFile(tmp.c_str(), "wb"));
        if (!out) return make_error(ErrorCode::IoError, std::format("cannot write '{}': {}", tmp, SDL_GetError()));
        std::vector<std::uint8_t> buf(1u << 20);
        for (;;) {
            const std::size_t n = SDL_ReadIO(in.get(), buf.data(), buf.size());
            if (n == 0) {
                if (SDL_GetIOStatus(in.get()) != SDL_IO_STATUS_EOF)
                    return make_error(ErrorCode::IoError, std::format("reading bundled '{}': {}", path, SDL_GetError()));
                break;
            }
            if (SDL_WriteIO(out.get(), buf.data(), n) != n) {
                const std::string why = SDL_GetError();
                out.reset();
                std::filesystem::remove(tmp, ec);
                return make_error(ErrorCode::IoError, std::format("writing '{}': {}", tmp, why));
            }
            total += n;
        }
        if (!SDL_FlushIO(out.get())) return make_error(ErrorCode::IoError, std::format("flushing '{}': {}", tmp, SDL_GetError()));
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return make_error(ErrorCode::IoError, std::format("cannot move '{}' into place", dest));
    }
    return total;
}

Result<std::vector<std::uint8_t>> Platform::read_bundled(const std::string& path) {
    const std::string full = bundled_path(path);
    std::size_t size = 0;
    void* data = SDL_LoadFile(full.c_str(), &size);
    if (!data) return make_error(ErrorCode::NotFound, std::format("bundled file '{}': {}", path, SDL_GetError()));
    std::vector<std::uint8_t> out(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
    SDL_free(data);
    return out;
}

bool Platform::is_mobile() noexcept {
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
    return true;
#else
    return false;
#endif
}

std::string Platform::clipboard() const {
    char* t = SDL_GetClipboardText();
    std::string s = t ? t : "";
    SDL_free(t);
    return s;
}
void Platform::set_clipboard(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

Result<std::unique_ptr<Window>> Platform::create_window(const WindowDesc& desc) {
    if (!desc_.video) return make_error(ErrorCode::Unsupported, "cannot create a window on a headless platform");
    SDL_WindowFlags flags = 0;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.maximized) flags |= SDL_WINDOW_MAXIMIZED;
    if (desc.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (desc.high_dpi) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* w = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (!w) return make_error(ErrorCode::DeviceError, std::format("SDL_CreateWindow failed: {}", SDL_GetError()));
    return std::unique_ptr<Window>(new Window(w));
}

void Platform::handle_gamepad_added(std::uint32_t id) {
    SDL_Gamepad* g = SDL_OpenGamepad(id);
    if (!g) return;
    for (std::size_t i = 0; i < InputState::kMaxGamepads; ++i) {
        GamepadState& s = input_.gamepad_mut(i);
        if (!s.connected) {
            s = GamepadState{};
            s.connected = true;
            s.id = id;
            const char* n = SDL_GetGamepadName(g);
            s.name = n ? n : "Gamepad";
            gamepads_.push_back(g);
            VICTUS_LOG_INFO("input", "gamepad {} connected: {}", i, s.name);
            return;
        }
    }
    SDL_CloseGamepad(g);
}

struct Platform::DialogState {
    struct Request {
        std::function<void(std::string)> done;
        std::string result;
        std::atomic<bool> finished{false};
    };
    std::mutex mutex;
    std::vector<std::shared_ptr<Request>> pending;
};

void Platform::show_folder_dialog(Window* parent, const std::string& default_location,
                                  std::function<void(std::string)> done) {
    if (!dialogs_) dialogs_ = std::make_shared<DialogState>();
    auto req = std::make_shared<DialogState::Request>();
    req->done = std::move(done);
    {
        std::lock_guard lock(dialogs_->mutex);
        dialogs_->pending.push_back(req);
    }
    // SDL may invoke the callback on another thread; it only records the result.
    auto* raw = new std::shared_ptr<DialogState::Request>(req);
    SDL_ShowOpenFolderDialog(
        [](void* userdata, const char* const* files, int) {
            auto* holder = static_cast<std::shared_ptr<DialogState::Request>*>(userdata);
            (*holder)->result = (files && files[0]) ? files[0] : "";
            (*holder)->finished.store(true, std::memory_order_release);
            delete holder;
        },
        raw, parent ? static_cast<SDL_Window*>(parent->native()) : nullptr,
        default_location.empty() ? nullptr : default_location.c_str(), false);
}

std::vector<Event> Platform::poll_events() {
    std::vector<Event> out;
    if (dialogs_) {
        std::vector<std::shared_ptr<DialogState::Request>> done;
        {
            std::lock_guard lock(dialogs_->mutex);
            auto& p = dialogs_->pending;
            for (auto it = p.begin(); it != p.end();) {
                if ((*it)->finished.load(std::memory_order_acquire)) {
                    done.push_back(*it);
                    it = p.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& r : done)
            if (r->done) r->done(r->result);
    }
    input_.begin_frame();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (native_hook_) native_hook_(&e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit_ = true;
            out.push_back({EventType::Quit});
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: out.push_back({EventType::WindowClose, e.window.windowID}); break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            out.push_back({EventType::WindowResized, e.window.windowID, e.window.data1, e.window.data2});
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: out.push_back({EventType::WindowFocusGained, e.window.windowID}); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: out.push_back({EventType::WindowFocusLost, e.window.windowID}); break;
        case SDL_EVENT_WINDOW_MINIMIZED: out.push_back({EventType::WindowMinimized, e.window.windowID}); break;
        case SDL_EVENT_WINDOW_RESTORED: out.push_back({EventType::WindowRestored, e.window.windowID}); break;
        case SDL_EVENT_DROP_FILE: {
            Event ev{EventType::FileDropped, e.drop.windowID};
            ev.path = e.drop.data ? e.drop.data : "";
            out.push_back(std::move(ev));
            break;
        }
        case SDL_EVENT_WILL_ENTER_BACKGROUND: out.push_back({EventType::AppWillEnterBackground}); break;
        case SDL_EVENT_DID_ENTER_FOREGROUND: out.push_back({EventType::AppDidEnterForeground}); break;
        case SDL_EVENT_LOW_MEMORY: out.push_back({EventType::AppLowMemory}); break;
        case SDL_EVENT_TERMINATING: out.push_back({EventType::AppTerminating}); break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: input_.set_key(map_scancode(e.key.scancode), e.type == SDL_EVENT_KEY_DOWN); break;
        case SDL_EVENT_TEXT_INPUT: input_.add_text(e.text.text); break;
        case SDL_EVENT_MOUSE_MOTION:
            input_.set_mouse_position({e.motion.x, e.motion.y});
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (const int b = map_mouse(e.button.button); b >= 0)
                input_.set_mouse_button(static_cast<MouseButton>(b), e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        case SDL_EVENT_MOUSE_WHEEL: input_.add_wheel(e.wheel.y); break;
        case SDL_EVENT_GAMEPAD_ADDED: handle_gamepad_added(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (std::size_t i = 0; i < InputState::kMaxGamepads; ++i) {
                GamepadState& s = input_.gamepad_mut(i);
                if (s.connected && s.id == e.gdevice.which) {
                    s.connected = false;
                    VICTUS_LOG_INFO("input", "gamepad {} disconnected", i);
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            for (std::size_t i = 0; i < InputState::kMaxGamepads; ++i) {
                GamepadState& s = input_.gamepad_mut(i);
                if (s.connected && s.id == e.gbutton.which)
                    if (const int b = map_pad_button(e.gbutton.button); b >= 0)
                        s.buttons[static_cast<std::size_t>(b)] = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            for (std::size_t i = 0; i < InputState::kMaxGamepads; ++i) {
                GamepadState& s = input_.gamepad_mut(i);
                if (s.connected && s.id == e.gaxis.which && e.gaxis.axis < static_cast<int>(GamepadAxis::Count))
                    s.axes[e.gaxis.axis] = static_cast<float>(e.gaxis.value) / 32767.0f;
            }
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION: {
            auto& t = input_.touches_mut();
            bool found = false;
            for (auto& p : t)
                if (p.id == e.tfinger.fingerID) {
                    p.position = {e.tfinger.x, e.tfinger.y};
                    p.pressure = e.tfinger.pressure;
                    found = true;
                }
            if (!found) t.push_back({e.tfinger.fingerID, {e.tfinger.x, e.tfinger.y}, e.tfinger.pressure});
            break;
        }
        case SDL_EVENT_FINGER_UP: {
            auto& t = input_.touches_mut();
            std::erase_if(t, [&](const TouchPoint& p) { return p.id == e.tfinger.fingerID; });
            break;
        }
        default: break;
        }
    }
    return out;
}

} // namespace victus::platform
