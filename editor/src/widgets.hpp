// SPDX-License-Identifier: Apache-2.0
// VICTUS Studio widget kit: vector icons drawn with ImDrawList (crisp at any scale, no icon
// font or texture), plus a few composite widgets that every panel shares so the Studio looks
// and behaves the same everywhere. Colours come from victus/ui/style.hpp only.
#pragma once

#include "victus/assets/cooked.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>

#include <string>
#include <string_view>

namespace victus::editor::kit {

enum class Icon {
    Play,
    Pause,
    Stop,
    Record,
    Step,
    Save,
    Undo,
    Redo,
    Search,
    Plus,
    Close,
    Check,
    Gear,
    Help,
    Export,
    Move,
    Rotate,
    Scale,
    Globe,
    Cube,
    Magnet,
    Focus,
    Folder,
    FolderOpen,
    File,
    Scene,
    Entity,
    Camera,
    Light,
    Mesh,
    Sprite,
    Texture,
    Model,
    Material,
    Shader,
    Script,
    Graph,
    Audio,
    Prefab,
    Animation,
    Terrain,
    Tileset,
    Sequence,
    Tree,
    Font,
    Particles,
    Physics,
    Ui,
    Info,
    Warning,
    Error,
    Eye,
    EyeOff,
    Lock,
    Network,
    Layout,
    Problems,
    Rocket,
};

/// Draws `icon` into the square [min, min + size) with colour `col`.
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 min, float size, ImU32 col);

/// An icon sized to the current text line at the cursor (then advances like Text).
void icon(Icon icon, ImVec4 col = ui::tokens::kTextMuted);

/// A square, frame-height icon button. `selected` draws the accent highlight; disabled buttons
/// still show their tooltip so users learn why. Returns true when clicked.
bool icon_button(const char* id, Icon icon, const char* tooltip, bool selected = false, bool enabled = true,
                 ImVec4 tint = ui::tokens::kText);

/// A button with a leading icon and a text label. `primary` uses the accent fill.
bool icon_text_button(const char* label, Icon icon, bool primary = false, bool enabled = true, ImVec2 size = {0, 0});

/// Bold section heading (Inter Bold at `scale` x the base size) with optional muted subtitle.
void heading(std::string_view text, float scale = ui::tokens::kTypeHeading);
void subtitle(std::string_view text);

/// Small rounded label with a tone colour, e.g. "Playing", "3 errors".
void pill(std::string_view text, ui::Tone tone);

/// Centered empty-state block for panels with nothing to show: icon, title, body, and an
/// optional action button. Returns true when the action was clicked.
bool empty_state(Icon icon, std::string_view title, std::string_view body, const char* action = nullptr);

/// A search field with a magnifier inside it and a clear button while it holds text; Escape
/// clears it too. `width` <= 0 fills the remaining width. Returns true when the text changed.
bool search_input(const char* id, const char* hint, std::string& text, float width = -1.0f);

/// Turns code names into words for labels: "linear_damping" -> "Linear Damping",
/// "RigidBody2D" -> "Rigid Body 2D", "UiButton" -> "UI Button". Text that already has spaces or
/// starts with a capital and has no '_' is returned unchanged (authored labels win).
[[nodiscard]] std::string humanize(std::string_view code);

/// True if `needle` (empty matches) is in `hay`, ignoring ASCII case.
[[nodiscard]] bool contains_icase(std::string_view hay, std::string_view needle) noexcept;

/// Tooltip shown on hover (also when the item is disabled).
void tooltip(const char* text);

/// Fonts installed by `install_fonts` (null until then, or when loading failed).
[[nodiscard]] ImFont* body_font();
[[nodiscard]] ImFont* bold_font();

/// Adds Inter Medium (default) and Inter Bold, with Latin Extended, Cyrillic and Greek merged
/// in, to the current ImGui context. Returns false (keeping the built-in font) on failure.
bool install_fonts();

/// Icon and tint for an asset kind (Assets panel, pickers, hierarchy).
[[nodiscard]] Icon icon_for(assets::AssetKind kind) noexcept;
[[nodiscard]] ImVec4 tint_for(assets::AssetKind kind) noexcept;

} // namespace victus::editor::kit
