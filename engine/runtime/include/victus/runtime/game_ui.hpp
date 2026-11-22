// SPDX-License-Identifier: Apache-2.0
// Draws a scene's in-game UI (victus::gameui canvases) on top of a rendered frame.
//
// Layout and interaction live in the headless gameui module; this renderer turns its draw items
// into anti-aliased vector geometry (rounded rectangles, borders, images, text) through a private
// Dear ImGui draw list and the VICTUS RHI UI pipeline. Text is shaped and laid out by
// victus::text (HarfBuzz + Unicode bidi, ADR-0224) with the embedded Inter fonts (Medium/Bold)
// and Noto Sans Bengali/Arabic fallbacks (SIL OFL 1.1), or a project font asset in front of
// them, and drawn from a glyph atlas rasterised at the exact pixel size.
#pragma once
#include "victus/gameui/gameui.hpp"
#include "victus/runtime/scene_rendering.hpp"
#include "victus/text/text.hpp"

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawList;

namespace victus::ui {
class UiContext;
}

namespace victus::runtime {

struct GameUiStats {
    std::uint32_t items{0};      // draw items submitted
    std::uint32_t vertices{0};
    std::uint32_t fonts{0};      // fonts loaded (built-in + project)
    std::uint32_t missing{0};    // images or fonts that could not be loaded this frame
    std::uint32_t glyphs{0};     // glyph quads drawn (shadows included)
    std::uint32_t missing_glyphs{0}; // characters no font in the stack could draw
    std::uint32_t atlas_glyphs{0};   // glyphs resident in the glyph atlas
    std::uint32_t atlas_uploads{0};  // glyph atlas texture uploads this frame (0 or 1)
    std::uint32_t atlas_width{0};    // glyph atlas texture size this frame
    std::uint32_t atlas_height{0};
    std::uint32_t layouts{0};        // shaped layouts in the cache
};

class GameUiRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<GameUiRenderer>> create(render::GpuContext& gpu, rhi::Format target_format);
    ~GameUiRenderer();
    GameUiRenderer(const GameUiRenderer&) = delete;
    GameUiRenderer& operator=(const GameUiRenderer&) = delete;

    // Lays the scene's UI out for a width x height target and draws it over `target` (whose
    // contents are kept). Records nothing when no canvas is visible.
    [[nodiscard]] Result<GameUiStats> render(rhi::CommandBuffer& cmd, scene::Scene& scene, GpuAssetCache& assets,
                                             rhi::TextureHandle target, std::uint32_t width, std::uint32_t height);

private:
    GameUiRenderer() = default;
    const text::FontStack& stack_for(const gameui::DrawItem& item, bool bold, GameUiStats& stats);
    void upload_atlas(rhi::CommandBuffer& cmd, GameUiStats& stats);
    const text::Layout& layout_for(const gameui::DrawItem& item, GameUiStats& stats);
    // Draws `layout` in item.rect (alignment, vertical alignment, caret scrolling); `colors` maps
    // Glyph::style to a colour (rich text), else every glyph uses item.color.
    void draw_text(ImDrawList* dl, const gameui::DrawItem& item, const text::Layout& layout, std::span<const std::uint32_t> colors,
                   GameUiStats& stats);

    render::GpuContext* gpu_{nullptr};
    GpuAssetCache* assets_{nullptr}; // set per render (project font lookup)
    std::unique_ptr<ui::UiContext> ui_;
    text::FontStack medium_, bold_; // built-in: Inter + Noto Bengali/Arabic fallbacks
    struct ProjectFont {
        text::FontStack stack; // project face, then the built-in Medium stack; empty = failed
    };
    std::unordered_map<std::string, ProjectFont> fonts_; // by asset path
    // Starts at 256x256 (256 KiB as RGBA8) and doubles up to 2048x2048 as scripts need glyphs.
    text::GlyphAtlas atlas_{256, 256, 2048, 2048};
    rhi::TextureHandle atlas_texture_;
    std::vector<std::uint8_t> atlas_rgba_;
    struct CachedLayout {
        text::Layout layout;
        std::uint64_t frame{0};
    };
    std::unordered_map<std::string, CachedLayout> layouts_; // by font stack, size, wrap, direction and text
    std::uint64_t frame_{0};
    std::vector<gameui::TextRun> runs_;
    std::vector<gameui::DrawItem> items_;
};

} // namespace victus::runtime
