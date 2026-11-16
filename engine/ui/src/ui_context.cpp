// SPDX-License-Identifier: Apache-2.0
#include "victus/ui/ui.hpp"

#include "victus/core/assert.hpp"
#include "victus/core/log.hpp"
#include "victus/platform/platform.hpp"

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cstring>

namespace victus::ui {

static_assert(sizeof(ImDrawIdx) == 2, "UI renderer binds 16-bit indices");
static_assert(sizeof(ImDrawVert) == 20, "UI renderer vertex layout");

ImTextureID UiContext::texture_id(rhi::TextureHandle texture) noexcept {
    if (!texture.valid()) return ImTextureID_Invalid;
    return (static_cast<ImTextureID>(texture.generation) << 32) | static_cast<ImTextureID>(texture.index);
}

rhi::TextureHandle UiContext::texture_handle(ImTextureID id) noexcept {
    rhi::TextureHandle h;
    h.index = static_cast<std::uint32_t>(id & 0xffffffffu);
    h.generation = static_cast<std::uint32_t>(id >> 32);
    return h;
}

Result<std::unique_ptr<UiContext>> UiContext::create(render::GpuContext& gpu, rhi::Format target_format,
                                                     platform::Platform* platform, platform::Window* window,
                                                     const UiOptions& options) {
    std::unique_ptr<UiContext> ui(new UiContext(gpu, platform, window));
    IMGUI_CHECKVERSION();
    ui->context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui->context_);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "victus_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (options.docking) io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ui->ini_path_ = options.ini_path;
    io.IniFilename = ui->ini_path_.empty() ? nullptr : ui->ini_path_.c_str();

    const float scale = window ? std::max(window->display_scale(), 1.0f) : 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    if (options.victus_theme) apply_victus_theme(style);
    else ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(scale);
    style.FontSizeBase = options.font_size;
    style.FontScaleDpi = scale;
    ImFontConfig font;
    font.SizePixels = options.font_size;
    io.Fonts->AddFontDefaultVector(&font);

    if (window) {
        if (!ImGui_ImplSDL3_InitForOther(static_cast<SDL_Window*>(window->native()))) {
            ImGui::DestroyContext(ui->context_);
            ui->context_ = nullptr;
            return Error{ErrorCode::DeviceError, "ImGui SDL3 platform backend initialisation failed"};
        }
        if (platform) {
            ImGuiContext* ctx = ui->context_;
            platform->set_native_event_hook([ctx](const void* e) {
                ImGuiContext* previous = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(ctx);
                ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(e));
                ImGui::SetCurrentContext(previous);
            });
        }
    }

    auto vs = gpu.shader("sprite.vert");
    if (!vs) return std::move(vs).error();
    auto fs = gpu.shader("sprite.frag");
    if (!fs) return std::move(fs).error();
    rhi::PipelineDesc p;
    p.name = "ui";
    p.vertex = vs.value();
    p.fragment = fs.value();
    p.vertex_buffers = {{0, sizeof(ImDrawVert), false}};
    p.attributes = {{0, 0, rhi::VertexFormat::Float2, offsetof(ImDrawVert, pos)},
                    {1, 0, rhi::VertexFormat::Float2, offsetof(ImDrawVert, uv)},
                    {2, 0, rhi::VertexFormat::UByte4Norm, offsetof(ImDrawVert, col)}};
    p.cull = rhi::CullMode::None;
    p.color_targets = {{target_format, rhi::BlendMode::Alpha}};
    auto pipe = gpu.device().create_pipeline(p);
    if (!pipe) return std::move(pipe).error();
    ui->pipeline_ = pipe.value();
    return ui;
}

UiContext::~UiContext() {
    auto& d = gpu_.device();
    if (context_) {
        ImGui::SetCurrentContext(context_);
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->RefCount == 1 && tex->TexID != ImTextureID_Invalid) {
                d.destroy(texture_handle(tex->TexID));
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        if (window_) {
            if (platform_) platform_->set_native_event_hook({});
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext(context_);
    }
    if (pipeline_) d.destroy(pipeline_);
    if (vertex_buffer_) d.destroy(vertex_buffer_);
    if (index_buffer_) d.destroy(index_buffer_);
}

void UiContext::new_frame(float dt, math::Vec2 display_size, float framebuffer_scale) {
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    if (window_) {
        ImGui_ImplSDL3_NewFrame();
    } else {
        io.DisplaySize = ImVec2(display_size.x, display_size.y);
        io.DisplayFramebufferScale = ImVec2(framebuffer_scale, framebuffer_scale);
        io.DeltaTime = dt > 0 ? dt : 1.0f / 60.0f;
    }
    prepared_ = false;
    ImGui::NewFrame();
}

bool UiContext::wants_mouse() const noexcept { return context_ && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }
bool UiContext::wants_keyboard() const noexcept {
    return context_ && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard;
}

void UiContext::handle_texture(ImTextureData& tex, rhi::CommandBuffer& cmd) {
    auto& d = gpu_.device();
    if (tex.Status == ImTextureStatus_WantDestroy && tex.UnusedFrames > 0) {
        if (tex.TexID != ImTextureID_Invalid) d.destroy(texture_handle(tex.TexID));
        tex.SetTexID(ImTextureID_Invalid);
        tex.SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    if (tex.Status != ImTextureStatus_WantCreate && tex.Status != ImTextureStatus_WantUpdates) return;
    if (tex.Status == ImTextureStatus_WantCreate) {
        rhi::TextureDesc desc;
        desc.name = "ui_texture";
        desc.format = rhi::Format::RGBA8_UNORM;
        desc.width = static_cast<std::uint32_t>(tex.Width);
        desc.height = static_cast<std::uint32_t>(tex.Height);
        desc.usage = rhi::TextureUsageSampled;
        auto created = d.create_texture(desc);
        if (!created) {
            VICTUS_LOG_ERROR("ui", "UI texture creation failed: {}", created.error().message);
            return;
        }
        tex.SetTexID(texture_id(created.value()));
    }
    // Full re-upload: atlas updates are rare (new glyph sizes) and the RHI uploads whole mips.
    const std::size_t pixels = static_cast<std::size_t>(tex.Width) * static_cast<std::size_t>(tex.Height);
    if (tex.Format == ImTextureFormat_RGBA32) {
        cmd.upload_texture(texture_handle(tex.TexID),
                           std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(tex.GetPixels()), pixels * 4));
    } else {
        scratch_.resize(pixels * 4);
        const auto* a = static_cast<const std::uint8_t*>(tex.GetPixels());
        for (std::size_t i = 0; i < pixels; ++i) {
            scratch_[i * 4 + 0] = 255;
            scratch_[i * 4 + 1] = 255;
            scratch_[i * 4 + 2] = 255;
            scratch_[i * 4 + 3] = a[i];
        }
        cmd.upload_texture(texture_handle(tex.TexID), scratch_);
    }
    ++stats_.texture_uploads;
    tex.SetStatus(ImTextureStatus_OK);
}

Status UiContext::ensure_buffers(std::uint32_t vertices, std::uint32_t indices) {
    auto& d = gpu_.device();
    if (vertices > vertex_capacity_) {
        if (vertex_buffer_) d.destroy(vertex_buffer_);
        vertex_capacity_ = std::max(vertices + vertices / 2, 4096u);
        auto b = d.create_buffer({"ui_vertices", vertex_capacity_ * static_cast<std::uint32_t>(sizeof(ImDrawVert)),
                                  rhi::BufferUsageVertex});
        if (!b) return std::move(b).error();
        vertex_buffer_ = b.value();
    }
    if (indices > index_capacity_) {
        if (index_buffer_) d.destroy(index_buffer_);
        index_capacity_ = std::max(indices + indices / 2, 8192u);
        // Keep the byte size a multiple of 4 for copy alignment.
        index_capacity_ = (index_capacity_ + 1u) & ~1u;
        auto b = d.create_buffer(
            {"ui_indices", index_capacity_ * static_cast<std::uint32_t>(sizeof(ImDrawIdx)), rhi::BufferUsageIndex});
        if (!b) return std::move(b).error();
        index_buffer_ = b.value();
    }
    return {};
}

Status UiContext::prepare(rhi::CommandBuffer& cmd) {
    ImGui::SetCurrentContext(context_);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    stats_ = {};
    if (dd->Textures) {
        for (ImTextureData* tex : *dd->Textures) handle_texture(*tex, cmd);
    }
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->Status == ImTextureStatus_OK) ++stats_.textures;

    vertices_.clear();
    indices_.clear();
    vertices_.reserve(static_cast<std::size_t>(dd->TotalVtxCount));
    indices_.reserve(static_cast<std::size_t>(dd->TotalIdxCount) + 1);
    for (const ImDrawList* list : dd->CmdLists) {
        vertices_.insert(vertices_.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
        indices_.insert(indices_.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
        stats_.draw_commands += static_cast<std::uint32_t>(list->CmdBuffer.Size);
    }
    stats_.draw_lists = static_cast<std::uint32_t>(dd->CmdLists.Size);
    stats_.vertices = static_cast<std::uint32_t>(vertices_.size());
    stats_.indices = static_cast<std::uint32_t>(indices_.size());
    if (indices_.size() % 2) indices_.push_back(0); // 4-byte aligned upload size
    if (!vertices_.empty()) {
        VICTUS_TRY_STATUS(ensure_buffers(static_cast<std::uint32_t>(vertices_.size()),
                                         static_cast<std::uint32_t>(indices_.size())));
        cmd.upload_buffer(vertex_buffer_, 0,
                          std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(vertices_.data()),
                                                        vertices_.size() * sizeof(ImDrawVert)));
        cmd.upload_buffer(index_buffer_, 0,
                          std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(indices_.data()),
                                                        indices_.size() * sizeof(ImDrawIdx)));
    }
    prepared_ = true;
    return {};
}

void UiContext::render(rhi::RenderPass& pass, rhi::CommandBuffer& cmd) {
    VICTUS_ASSERT(prepared_, "UiContext::prepare must run before render");
    ImGui::SetCurrentContext(context_);
    ImDrawData* dd = ImGui::GetDrawData();
    const float fb_w = dd->DisplaySize.x * dd->FramebufferScale.x;
    const float fb_h = dd->DisplaySize.y * dd->FramebufferScale.y;
    if (fb_w <= 0 || fb_h <= 0 || stats_.vertices == 0) return;

    const math::Mat4 proj = math::Mat4::orthographic(dd->DisplayPos.x, dd->DisplayPos.x + dd->DisplaySize.x,
                                                     dd->DisplayPos.y + dd->DisplaySize.y, dd->DisplayPos.y, -1.0f, 1.0f);
    rhi::TextureHandle bound;
    const auto setup_state = [&] {
        pass.bind_pipeline(pipeline_);
        pass.set_viewport({0, 0, fb_w, fb_h, 0, 1});
        cmd.push_uniforms(rhi::ShaderStage::Vertex, 0, proj);
        pass.bind_vertex_buffer(0, vertex_buffer_);
        pass.bind_index_buffer(index_buffer_, rhi::IndexType::U16);
        bound = {};
    };
    setup_state();
    const ImDrawCallback reset_callback = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;

    const ImVec2 clip_off = dd->DisplayPos;
    const ImVec2 clip_scale = dd->FramebufferScale;
    std::uint32_t global_vtx = 0, global_idx = 0;
    for (const ImDrawList* list : dd->CmdLists) {
        for (const ImDrawCmd& c : list->CmdBuffer) {
            if (c.UserCallback) {
                if (reset_callback && c.UserCallback == reset_callback) setup_state();
                else c.UserCallback(list, &c);
                continue;
            }
            const float x0 = std::max((c.ClipRect.x - clip_off.x) * clip_scale.x, 0.0f);
            const float y0 = std::max((c.ClipRect.y - clip_off.y) * clip_scale.y, 0.0f);
            const float x1 = std::min((c.ClipRect.z - clip_off.x) * clip_scale.x, fb_w);
            const float y1 = std::min((c.ClipRect.w - clip_off.y) * clip_scale.y, fb_h);
            if (x1 <= x0 || y1 <= y0) continue;
            pass.set_scissor({static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
                              static_cast<std::int32_t>(x1 - x0), static_cast<std::int32_t>(y1 - y0)});
            rhi::TextureHandle tex = texture_handle(c.GetTexID());
            if (!gpu_.device().describe(tex)) tex = gpu_.white();
            if (!(tex == bound)) {
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, tex, gpu_.linear_clamp());
                bound = tex;
            }
            pass.draw_indexed(c.ElemCount, 1, c.IdxOffset + global_idx,
                              static_cast<std::int32_t>(c.VtxOffset + global_vtx));
        }
        global_vtx += static_cast<std::uint32_t>(list->VtxBuffer.Size);
        global_idx += static_cast<std::uint32_t>(list->IdxBuffer.Size);
    }
    pass.set_scissor({0, 0, static_cast<std::int32_t>(fb_w), static_cast<std::int32_t>(fb_h)});
}

Status UiContext::render_to(rhi::CommandBuffer& cmd, rhi::TextureHandle target, bool clear, math::Color clear_color) {
    VICTUS_TRY_STATUS(prepare(cmd));
    rhi::RenderPassDesc rp;
    rhi::ColorAttachment color;
    color.texture = target;
    color.load = clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
    color.clear = clear_color;
    rp.colors.push_back(color);
    auto pass = cmd.begin_render_pass(rp);
    render(pass, cmd);
    pass.end();
    return {};
}

} // namespace victus::ui
