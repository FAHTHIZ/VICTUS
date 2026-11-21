// SPDX-License-Identifier: Apache-2.0
// Draws a scene's in-game UI (victus::gameui canvases) on top of a rendered frame.
//
// Layout and interaction live in the headless gameui module; this renderer turns its draw items
// into anti-aliased vector geometry (rounded rectangles, borders, images, text) through a private
// Dear ImGui draw list and the VICTUS RHI UI pipeline. Text uses the embedded Inter fonts
// (Medium/Bold, SIL OFL 1.1) or a project font asset, rasterised at the exact pixel size.
#pragma once
#include "victus/gameui/gameui.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ImFont;
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
    ImFont* font_for(const gameui::DrawItem& item, GpuAssetCache& assets, GameUiStats& stats);
    void draw_rich_text(ImDrawList* dl, const gameui::DrawItem& item, ImFont* regular);

    render::GpuContext* gpu_{nullptr};
    std::unique_ptr<ui::UiContext> ui_;
    ImFont* medium_{nullptr};
    ImFont* bold_{nullptr};
    struct ProjectFont {
        std::vector<unsigned char> data; // kept alive for the atlas (not owned by it)
        ImFont* font{nullptr};
    };
    std::unordered_map<std::string, ProjectFont> fonts_; // by asset path; font null = failed
    std::vector<gameui::DrawItem> items_;
};

} // namespace victus::runtime
