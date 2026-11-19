// SPDX-License-Identifier: Apache-2.0
// Gameplay framework core: a phase-ordered system scheduler that advances a Scene with a fixed
// or variable time step, plus built-in behaviour components usable without scripting.
// Scripting (Luau) and native game modules register their systems through the same scheduler.
#pragma once
#include "victus/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::gameplay {

struct FrameTime {
    double time{0};          // seconds since play started
    float dt{0};             // seconds for this update
    std::uint64_t frame{0};  // update counter
};

enum class Phase : std::uint8_t { PreUpdate, FixedUpdate, Update, PostUpdate };

// Gamepad controls in the standard (Xbox-like) layout, by position: South = A / Cross, East =
// B / Circle, West = X / Square, North = Y / Triangle. Indices match platform::GamepadButton and
// platform::GamepadAxis. Stick axes are -1..1 (+X right, +Y down), triggers 0..1.
inline constexpr int kGamepadButtonCount = 15;
inline constexpr int kGamepadAxisCount = 6;
inline constexpr int kMaxGamepads = 4;
// "South".."DpadRight" (also accepts "A", "B", "X", "Y", "LB", "RB", "Select"/"Menu"); -1 if unknown.
[[nodiscard]] int gamepad_button_index(std::string_view name) noexcept;
// "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger"/"LT", "RightTrigger"/"RT"; -1 if unknown.
[[nodiscard]] int gamepad_axis_index(std::string_view name) noexcept;
[[nodiscard]] const char* gamepad_button_name(int index) noexcept;
[[nodiscard]] const char* gamepad_axis_name(int index) noexcept;

// Platform-independent input queries for gameplay code and scripts. Keys use the platform key
// names ("A".."Z", "0".."9", "Space", "Enter", "Left", "Right", "Up", "Down", "Escape",
// "LeftShift", "F1".., see platform::key_name). Mouse buttons:
// 0 left, 1 middle, 2 right. Gamepads 0..kMaxGamepads-1; `previous` asks for last frame's state
// (used for action edges). Headless runs and servers have no input source.
// One finger on a touch screen. `position` is normalized to the window ([0,1], origin top-left) so
// scripts work at any resolution; `id` stays the same while the finger is down.
struct Touch {
    std::uint64_t id{0};
    math::Vec2 position{};
    float pressure{0};
};

class InputSource {
public:
    virtual ~InputSource() = default;
    [[nodiscard]] virtual bool key_down(std::string_view key) const = 0;
    [[nodiscard]] virtual bool key_pressed(std::string_view key) const = 0;
    [[nodiscard]] virtual bool key_released(std::string_view key) const = 0;
    [[nodiscard]] virtual bool mouse_down(int button) const = 0;
    [[nodiscard]] virtual bool mouse_pressed(int button) const = 0;
    [[nodiscard]] virtual math::Vec2 mouse_position() const = 0;
    [[nodiscard]] virtual math::Vec2 mouse_delta() const = 0;
    [[nodiscard]] virtual bool mouse_released(int /*button*/) const { return false; }
    // Wheel notches scrolled this frame (positive away from the user).
    [[nodiscard]] virtual float mouse_wheel() const { return 0; }
    [[nodiscard]] virtual bool gamepad_connected(int /*pad*/) const { return false; }
    [[nodiscard]] virtual bool gamepad_button(int /*pad*/, int /*button*/, bool /*previous*/ = false) const {
        return false;
    }
    [[nodiscard]] virtual float gamepad_axis(int /*pad*/, int /*axis*/, bool /*previous*/ = false) const { return 0; }
    // Fingers currently down, in the order they touched. Taps also arrive as left mouse clicks.
    [[nodiscard]] virtual int touch_count() const { return 0; }
    [[nodiscard]] virtual Touch touch(int /*index*/) const { return {}; }
};

// Programmatic input for tests, headless runs, bots and replays. `pressed`/`released` edges are
// derived from the previous call to end_frame(), so call it once after every Simulation::tick.
class ManualInput final : public InputSource {
public:
    void set_key(std::string_view key, bool down) {
        auto it = std::find(down_.begin(), down_.end(), key);
        if (down && it == down_.end()) down_.emplace_back(key);
        if (!down && it != down_.end()) down_.erase(it);
    }
    void set_mouse(int button, bool down) {
        if (button >= 0 && button < 8) mouse_[button] = down;
    }
    void set_mouse_position(math::Vec2 p) noexcept { mouse_pos_ = p; }
    void set_touches(std::vector<Touch> touches) { touches_ = std::move(touches); }
    [[nodiscard]] int touch_count() const override { return static_cast<int>(touches_.size()); }
    [[nodiscard]] Touch touch(int i) const override {
        return i >= 0 && i < touch_count() ? touches_[static_cast<std::size_t>(i)] : Touch{};
    }
    void set_gamepad_connected(int pad, bool connected) {
        if (pad >= 0 && pad < kMaxGamepads) pads_[static_cast<std::size_t>(pad)].connected = connected;
    }
    // Pressing a button or moving an axis also marks the pad connected.
    void set_gamepad_button(int pad, int button, bool down) {
        if (pad < 0 || pad >= kMaxGamepads || button < 0 || button >= kGamepadButtonCount) return;
        Pad& p = pads_[static_cast<std::size_t>(pad)];
        p.connected = true;
        p.buttons[static_cast<std::size_t>(button)] = down;
    }
    void set_gamepad_axis(int pad, int axis, float value) {
        if (pad < 0 || pad >= kMaxGamepads || axis < 0 || axis >= kGamepadAxisCount) return;
        Pad& p = pads_[static_cast<std::size_t>(pad)];
        p.connected = true;
        p.axes[static_cast<std::size_t>(axis)] = std::clamp(value, -1.0f, 1.0f);
    }
    void end_frame() {
        prev_ = down_;
        prev_mouse_ = mouse_;
        prev_pos_ = mouse_pos_;
        prev_pads_ = pads_;
        wheel_ = 0;
    }
    // Wheel notches for this frame (cleared by end_frame).
    void add_mouse_wheel(float notches) noexcept { wheel_ += notches; }
    [[nodiscard]] float mouse_wheel() const override { return wheel_; }
    [[nodiscard]] bool key_down(std::string_view k) const override { return has(down_, k); }
    [[nodiscard]] bool key_pressed(std::string_view k) const override { return has(down_, k) && !has(prev_, k); }
    [[nodiscard]] bool key_released(std::string_view k) const override { return !has(down_, k) && has(prev_, k); }
    [[nodiscard]] bool mouse_down(int b) const override { return b >= 0 && b < 8 && mouse_[b]; }
    [[nodiscard]] bool mouse_pressed(int b) const override { return mouse_down(b) && !prev_mouse_[b]; }
    [[nodiscard]] math::Vec2 mouse_position() const override { return mouse_pos_; }
    [[nodiscard]] math::Vec2 mouse_delta() const override {
        return {mouse_pos_.x - prev_pos_.x, mouse_pos_.y - prev_pos_.y};
    }
    [[nodiscard]] bool mouse_released(int b) const override { return b >= 0 && b < 8 && !mouse_[b] && prev_mouse_[b]; }
    [[nodiscard]] bool gamepad_connected(int pad) const override {
        return pad >= 0 && pad < kMaxGamepads && pads_[static_cast<std::size_t>(pad)].connected;
    }
    [[nodiscard]] bool gamepad_button(int pad, int button, bool previous = false) const override {
        if (pad < 0 || pad >= kMaxGamepads || button < 0 || button >= kGamepadButtonCount) return false;
        return (previous ? prev_pads_ : pads_)[static_cast<std::size_t>(pad)].buttons[static_cast<std::size_t>(button)];
    }
    [[nodiscard]] float gamepad_axis(int pad, int axis, bool previous = false) const override {
        if (pad < 0 || pad >= kMaxGamepads || axis < 0 || axis >= kGamepadAxisCount) return 0;
        return (previous ? prev_pads_ : pads_)[static_cast<std::size_t>(pad)].axes[static_cast<std::size_t>(axis)];
    }

private:
    struct Pad {
        bool connected{false};
        std::array<bool, kGamepadButtonCount> buttons{};
        std::array<float, kGamepadAxisCount> axes{};
    };
    static bool has(const std::vector<std::string>& v, std::string_view k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    }
    std::vector<std::string> down_, prev_;
    std::array<bool, 8> mouse_{}, prev_mouse_{};
    math::Vec2 mouse_pos_{}, prev_pos_{};
    float wheel_{0};
    std::array<Pad, kMaxGamepads> pads_{}, prev_pads_{};
    std::vector<Touch> touches_;
};

// Per-entity input routing, published as a scene world resource by network sessions: scripts
// on `entity` read input from route(entity, fallback) instead of the simulation's input. A
// server routes each client's input to the entities that client owns; a client gives entities
// it does not own no input. Returning `fallback` keeps the default; null means "no input".
struct InputRouting {
    std::function<const InputSource*(scene::Entity entity, const InputSource* fallback)> route;
};

using SystemFn = std::function<void(scene::Scene&, const FrameTime&)>;

struct SystemInfo {
    std::string name;
    Phase phase{Phase::Update};
    SystemFn fn;
    bool enabled{true};
    const char* zone{nullptr}; // profiler scope "system.<name>"
};

// Runs registered systems over a scene. Fixed-update systems run 0..max_steps times per tick
// at `fixed_dt` (accumulator); other phases run once per tick with the frame delta.
class Simulation {
public:
    explicit Simulation(float fixed_dt = 1.0f / 60.0f) : fixed_dt_(fixed_dt) {}
    void add_system(std::string name, Phase phase, SystemFn fn);
    [[nodiscard]] SystemInfo* find(std::string_view name);
    [[nodiscard]] const std::vector<SystemInfo>& systems() const noexcept { return systems_; }

    // Advances by `dt` seconds (clamped to max_frame_dt). Updates world transforms at the end.
    void tick(scene::Scene& scene, float dt);
    void reset() noexcept;

    [[nodiscard]] const FrameTime& time() const noexcept { return time_; }
    [[nodiscard]] std::uint64_t fixed_steps() const noexcept { return fixed_steps_; }
    [[nodiscard]] float fixed_dt() const noexcept { return fixed_dt_; }
    float max_frame_dt{0.25f};
    std::uint32_t max_fixed_steps{8};
    // Optional input for systems/scripts; owned by the host (player, editor) and may be null.
    const InputSource* input{nullptr};

private:
    void run_phase(scene::Scene& scene, Phase phase, const FrameTime& t);
    std::vector<SystemInfo> systems_;
    float fixed_dt_;
    double accumulator_{0};
    FrameTime time_;
    std::uint64_t fixed_steps_{0};
};

// ---- built-in behaviours -----------------------------------------------------------------------

// Spins the entity around a local axis.
struct RotatorComponent {
    math::Vec3 axis{0, 1, 0};
    float degrees_per_second{45.0f};
};

// Moves the entity sinusoidally around the position it had when play started.
struct OscillatorComponent {
    math::Vec3 amplitude{0, 0.5f, 0};
    float frequency{0.5f}; // Hz
    float phase{0.0f};     // radians
    // Runtime state (not serialized).
    math::Vec3 origin;
    bool has_origin{false};
};

// Per-entity adjustments applied on top of the entity's MeshRenderer material (its .vmat asset,
// the model's material or the built-in one), so one material can be shared by entities that
// scroll, pulse or glow differently. Serialized as "MaterialOverrides".
struct MaterialOverridesComponent {
    math::Vec2 uv_offset{0, 0};   // added to the material's UV offset
    math::Vec2 uv_scroll{0, 0};   // UV units per second (conveyor belts, water, lava)
    bool override_emissive{false};
    math::Color emissive{0, 0, 0, 1}; // linear; replaces the material's emissive colour when overridden
    float emissive_strength{1.0f};
    // Runtime state (not serialized): accumulated scroll in [0, 1).
    math::Vec2 scrolled{0, 0};
};

// Scene-wide look: sky/ambient lighting and post-processing controls. The first entity with
// this component drives the scene's environment; quality tiers decide which effects run.
struct EnvironmentComponent {
    enum class Tonemapper : std::uint8_t { None, Aces, Neutral, Reinhard };
    math::Color sky_zenith{0.18f, 0.36f, 0.75f, 1};
    math::Color sky_horizon{0.62f, 0.72f, 0.85f, 1};
    math::Color ground{0.22f, 0.2f, 0.18f, 1};
    float ambient_intensity{0.6f};
    bool draw_sky{true};
    math::Color clear_color{0.05f, 0.05f, 0.06f, 1};
    Tonemapper tonemapper{Tonemapper::Aces};
    float exposure{1.0f};
    float exposure_compensation{0.0f};
    bool auto_exposure{false};
    float auto_exposure_min_ev{-4.0f};
    float auto_exposure_max_ev{6.0f};
    float saturation{1.0f};
    float contrast{1.0f};
    float vignette{0.25f};
    float bloom_intensity{0.05f};
    float bloom_threshold{1.0f};
    float ao_intensity{1.0f};
    float ao_radius{0.6f};
    float shadow_distance{40.0f};
    enum class Fog : std::uint8_t { None, Linear, Exponential, ExponentialSquared };
    Fog fog{Fog::None};
    math::Color fog_color{0.62f, 0.72f, 0.85f, 1};
    float fog_start{50.0f};
    float fog_end{400.0f};
    float fog_density{0.004f};
    float fog_max_opacity{1.0f};
    float coverage_amount{0.0f};
    math::Color coverage_color{0.86f, 0.89f, 0.95f, 1};
    float coverage_roughness{0.8f};
    float coverage_slope{0.55f};
    // Image-based lighting from an HDR (.hdr, equirectangular) texture asset; nil = analytic sky.
    Uuid environment_map;
    float environment_intensity{1.0f};
    float environment_rotation{0.0f}; // degrees about +Y
    float sky_blur{0.0f};             // 0 sharp background .. 1 fully blurred
    // Physically based atmosphere driven by the directional light (used when environment_map is
    // empty): sky, image-based lighting and, optionally, the sun colour follow time of day.
    bool physical_sky{false};
    float atmosphere_density{1.0f};   // Rayleigh (air) density, 1 = Earth
    float atmosphere_haze{1.0f};      // Mie (aerosol) density, 1 = clear day
    float atmosphere_intensity{1.0f}; // brightness of sky and IBL relative to the sun
    bool atmosphere_tints_sun{true};  // sun light colour = atmospheric transmittance (red sunsets)
    // Volumetric fog (ADR-0098): lit, shadowed height fog in a froxel grid (render::VolumetricFog).
    bool volumetric_fog{false};
    float volumetric_density{0.02f};       // extinction per metre at and below volumetric_height
    float volumetric_height{0.0f};         // world height where the falloff starts
    float volumetric_falloff{0.1f};        // per metre above it (0 = uniform)
    math::Color volumetric_albedo{1, 1, 1, 1};
    float volumetric_anisotropy{0.6f};     // forward scattering (glow towards lights)
    float volumetric_distance{80.0f};      // metres covered
    float volumetric_sun{1.0f};            // light source multipliers
    float volumetric_ambient{1.0f};
    float volumetric_local_lights{1.0f};
    float volumetric_temporal{0.9f};       // history weight (0 = no temporal filtering)
    // Volumetric clouds (ADR-0111): a ray-marched cloud layer lit by the directional light.
    bool clouds{false};
    float cloud_coverage{0.5f};            // 0 clear .. 1 overcast
    float cloud_density{0.03f};            // extinction per metre inside dense cloud
    float cloud_bottom{1500.0f};           // layer base height (m)
    float cloud_thickness{2000.0f};        // layer depth (m)
    float cloud_size{4000.0f};             // cloud shape size (m)
    float cloud_detail{0.35f};             // edge erosion 0..1
    math::Vec3 cloud_wind{10.0f, 0.0f, 4.0f}; // drift, m/s
    float cloud_anisotropy{0.6f};          // silver lining towards the sun
    float cloud_sun{1.0f};                 // light multipliers
    float cloud_ambient{1.0f};
    float cloud_distance{30000.0f};        // metres of view ray covered
};

// Box of extra volumetric fog centred on the entity (ADR-0098), size scaled by the entity's
// transform; only visible while the scene's Environment has volumetric_fog on.
struct FogVolumeComponent {
    math::Vec3 size{4.0f, 4.0f, 4.0f}; // metres
    float density{0.1f};               // extinction per metre added inside
    math::Color albedo{1, 1, 1, 1};
    float edge_fade{0.1f};             // fade width as a fraction of the size (0 hard .. 0.5)
    bool visible{true};
};

// Local reflection probe: captures the scene around the entity once (and again when moved or
// when `revision` changes) into a prefiltered cubemap; objects whose centre is inside the
// world-aligned box get box-projected reflections and the probe's diffuse light.
struct ReflectionProbeComponent {
    math::Vec3 size{10.0f, 5.0f, 10.0f}; // box extents in metres, centred on the entity
    math::Vec3 capture_offset{};         // capture point relative to the entity (inside the box)
    float intensity{1.0f};
    std::uint32_t resolution{128};       // face size of the capture (16..512)
    std::uint32_t revision{0};           // change to force a recapture
};

// A box of irradiance probes centred on the entity (ADR-0056): baked progressively from scene captures,
// then lit surfaces inside take their diffuse ambient light from it instead of the sky.
struct IrradianceVolumeComponent {
    math::Vec3 size{20.0f, 8.0f, 20.0f}; // world-aligned box in metres, centred on the entity
    float spacing{2.5f};                 // target distance between probes (counts are capped per axis)
    float intensity{1.0f};
    float normal_offset{0.25f};          // lookup offset along the surface normal (reduces leaking)
    std::uint32_t resolution{16};        // capture face size per probe (16..128)
    std::uint32_t bounces{2};            // bake passes: 1 = direct light only, each extra pass adds a bounce
    std::uint32_t revision{0};           // change to force a rebake
    // Dynamic GI (ADR-0057, High/Ultra): after the bake, keep re-capturing probes round-robin so
    // the volume follows lighting changes. `dynamic_blend` is the weight of each new capture.
    bool dynamic{false};
    float dynamic_blend{0.5f};
};

// Projected decal (ADR-0064): a box centred on the entity (size scaled by the entity's scale)
// projects `texture` along the entity's local -Z axis onto the opaque surfaces inside it.
struct DecalComponent {
    enum class Blend : std::uint8_t { Multiply, Alpha, Additive }; // matches render::DecalBlend
    Uuid texture;                       // nil = solid colour
    math::Vec3 size{1.0f, 1.0f, 0.5f};  // box width, height, projection depth (metres)
    math::Color color{1, 1, 1, 1};      // linear tint; alpha = opacity
    Blend blend{Blend::Multiply};
    math::Vec2 uv_min{0, 0};            // texture region (top-left)
    math::Vec2 uv_max{1, 1};            // texture region (bottom-right)
    float angle_fade{0.3f};             // 0 = every surface .. 1 = only surfaces facing the axis
    float depth_fade{0.2f};             // fade over this fraction of the depth at the box ends
    std::int32_t sort_order{0};         // higher draws over lower
    bool visible{true};
};

// 2D light (ADR-0065). While a scene has an enabled Light2D, lit sprites, tilemaps and alpha 2D
// particles are multiplied by the sum of the Global lights (ambient) and the Point/Spot lights
// covering them; without any, 2D scenes stay unlit. Spots shine along the entity's local -Y axis.
struct Light2DComponent {
    enum class Type : std::uint8_t { Point, Spot, Global };
    Type type{Type::Point};
    math::Color color{1, 1, 1, 1};
    float intensity{1.0f};
    float radius{5.0f};               // world units (scaled by the entity's largest XY scale)
    float inner_angle_degrees{25.0f}; // spot half-angles: full light inside inner,
    float outer_angle_degrees{40.0f}; // none beyond outer
    float falloff{1.0f};              // < 1 broader, > 1 tighter
    bool enabled{true};
};

// Registers serializers ("Rotator", "Oscillator", "Environment"). Idempotent; call once at startup before
// loading scenes so these components are not treated as unknown.
void register_components();
// Adds the built-in behaviour systems to a simulation.
void add_builtin_systems(Simulation& sim);

} // namespace victus::gameplay
