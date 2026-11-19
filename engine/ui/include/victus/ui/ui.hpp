// SPDX-License-Identifier: Apache-2.0
// VICTUS immediate-mode UI layer: owns a Dear ImGui context, feeds it platform input and
// renders it through the VICTUS RHI (no upstream renderer backend). Used by the editor and
// available to games for debug/tool UI.
#pragma once
#include "victus/core/result.hpp"
#include "victus/render/gpu_context.hpp"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace victus::platform {
class Platform;
class Window;
} // namespace victus::platform

namespace victus::ui {

struct UiOptions {
    float font_size{15.0f};     // logical pixels; multiplied by the window display scale
    bool docking{true};
    std::string ini_path;       // empty: layout is not persisted
    bool victus_theme{true};
    // A TrueType/OpenType font to use instead of the built-in vector font (wider glyph coverage: dashes,
    // middle dots, degree signs, accents). Empty: the built-in font. Read with Platform::read_bundled by the caller.
    std::vector<std::uint8_t> font_data;
};

struct UiStats {
    std::uint32_t draw_lists{0}, draw_commands{0}, vertices{0}, indices{0};
    std::uint32_t textures{0}, texture_uploads{0};
};

class UiContext {
public:
    // `platform`/`window` may be null for offscreen use (tests, thumbnails); the caller then
    // supplies the display size to `new_frame`.
    [[nodiscard]] static Result<std::unique_ptr<UiContext>> create(render::GpuContext& gpu, rhi::Format target_format,
                                                                   platform::Platform* platform,
                                                                   platform::Window* window,
                                                                   const UiOptions& options = {});
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    // Starts a UI frame. With a window, size/scale/delta come from the platform; otherwise from
    // the arguments.
    void new_frame(float dt, math::Vec2 display_size = {}, float framebuffer_scale = 1.0f);
    // Finishes the frame and records texture + geometry uploads. Call outside render passes.
    [[nodiscard]] Status prepare(rhi::CommandBuffer& cmd);
    // Records the UI draw commands into an active pass whose target has `target_format`.
    void render(rhi::RenderPass& pass, rhi::CommandBuffer& cmd);
    // Convenience: prepare + a render pass on `target` (load existing contents or clear).
    [[nodiscard]] Status render_to(rhi::CommandBuffer& cmd, rhi::TextureHandle target, bool clear,
                                   math::Color clear_color = {0, 0, 0, 1});

    [[nodiscard]] static ImTextureID texture_id(rhi::TextureHandle texture) noexcept;
    [[nodiscard]] static rhi::TextureHandle texture_handle(ImTextureID id) noexcept;

    // The window and docking layout as ImGui ini text, and replacing it: the new layout takes
    // effect at the start of the next new_frame (docking cannot change mid-frame).
    [[nodiscard]] std::string layout_snapshot() const;
    void load_layout(std::string ini) { pending_layout_ = std::move(ini); }

    [[nodiscard]] bool wants_mouse() const noexcept;
    [[nodiscard]] bool wants_keyboard() const noexcept;
    [[nodiscard]] ImGuiContext* imgui() const noexcept { return context_; }
    [[nodiscard]] const UiStats& stats() const noexcept { return stats_; }

private:
    UiContext(render::GpuContext& gpu, platform::Platform* platform, platform::Window* window)
        : gpu_(gpu), platform_(platform), window_(window) {}
    void handle_texture(ImTextureData& tex, rhi::CommandBuffer& cmd);
    [[nodiscard]] Status ensure_buffers(std::uint32_t vertices, std::uint32_t indices);

    render::GpuContext& gpu_;
    platform::Platform* platform_;
    platform::Window* window_;
    ImGuiContext* context_{nullptr};
    std::string ini_path_;
    std::string pending_layout_;
    rhi::PipelineHandle pipeline_;
    bool target_srgb_{false};
    rhi::BufferHandle vertex_buffer_, index_buffer_;
    std::uint32_t vertex_capacity_{0}, index_capacity_{0};
    std::vector<ImDrawVert> vertices_;
    std::vector<ImDrawIdx> indices_;
    std::vector<std::uint8_t> scratch_;
    bool prepared_{false};
    UiStats stats_;
};

// Applies the VICTUS dark editor theme to the current ImGui style.
void apply_victus_theme(ImGuiStyle& style);

} // namespace victus::ui
