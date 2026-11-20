// SPDX-License-Identifier: Apache-2.0
// OpenGL ES 3.0 implementation of the VICTUS RHI (ADR-0144). Internal to the rhi module: the
// public Device/CommandBuffer/RenderPass methods in rhi_sdl.cpp forward here when the device was
// created on GLES.
//
// Conventions match the SDL_GPU backends, so the renderer needs no GL-specific code:
//  * Clip space is D3D/Vulkan style (z in [0, w], y up). GLSL ES vertex shaders handed to this
//    backend must already map it to GL ([-w, w] depth, y flipped), which is what
//    `spirv-cross --fixup-clipspace --flip-vert-y` produces. Rendering upside down makes row 0 of
//    every render target the top row, exactly as uploads and readback expect, and keeps
//    gl_FragCoord.y equal to SDL_GPU's pixel row. The front face is mirrored to compensate, and the
//    window is presented with a vertical flip.
//  * Resource slots: the source declares them in comment lines, one per resource:
//      //! sampler <slot> <glsl name>      combined image samplers, SDL_GPU sampler slots
//      //! ubo <slot> <block name>         uniform blocks, SDL_GPU push-uniform slots
//      //! storage <slot> <glsl name>      read-only storage buffers, as a highp usampler2D
//    ES 3.00 has no storage buffers, so each BufferUsageStorageRead buffer is mirrored into an
//    RGBA32UI texture, 1024 texels (16 bytes each) per row: byte offset 16 * i is texel
//    (i % 1024, i / 1024). Storage slots use the texture units after the stage's samplers.
//    Fragment samplers use texture units 0-15, vertex samplers 16-31; vertex uniform slots use
//    binding points 0-3, fragment slots 4-7.
//  * There is no compute and no indirect draws on the GPU: indirect draws are replayed from a
//    CPU copy of the argument buffer.
//  * The device is single-threaded: every call must come from the thread that created it.
#pragma once

#include "victus/rhi/rhi.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace victus::rhi {

class GlesBackend {
public:
    // Creates the GL ES 3.0 context (on a hidden 1x1 window) and fills `caps`.
    [[nodiscard]] static Result<std::unique_ptr<GlesBackend>> create(Device& device, const DeviceDesc& desc,
                                                                     DeviceCapabilities& caps);
    ~GlesBackend();
    GlesBackend(const GlesBackend&) = delete;
    GlesBackend& operator=(const GlesBackend&) = delete;

    [[nodiscard]] void* context() const noexcept { return context_; }

    Status attach_window(platform::Window& window, PresentMode mode);
    void detach_window(platform::Window& window);
    [[nodiscard]] bool supports_format(Format format, std::uint32_t usage) const;

    Result<TextureHandle> create_texture(const TextureDesc& desc);
    Result<BufferHandle> create_buffer(const BufferDesc& desc);
    Result<SamplerHandle> create_sampler(const SamplerDesc& desc);
    Result<ShaderHandle> create_shader(const ShaderDesc& desc);
    Result<PipelineHandle> create_pipeline(const PipelineDesc& desc);
    void destroy(TextureHandle h);
    void destroy(BufferHandle h);
    void destroy(SamplerHandle h);
    void destroy(ShaderHandle h);
    void destroy(PipelineHandle h);
    [[nodiscard]] const TextureDesc* describe(TextureHandle h) const;

    void wait_idle();
    Result<std::vector<std::uint8_t>> read_texture(TextureHandle h);
    Result<std::vector<std::uint8_t>> read_buffer(BufferHandle h, std::uint32_t offset, std::uint32_t size);

    // Command recording executes immediately on GL.
    void begin_commands();
    void push_uniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size);
    void upload_buffer(BufferHandle buffer, std::uint32_t offset, std::span<const std::uint8_t> data, bool discard);
    void upload_texture(TextureHandle texture, std::span<const std::uint8_t> pixels, std::uint32_t mip, std::uint32_t layer);
    void generate_mipmaps(TextureHandle texture);
    TextureHandle acquire_swapchain(platform::Window& window, std::uint32_t& width, std::uint32_t& height);
    bool begin_render_pass(const RenderPassDesc& desc);
    void blit(TextureHandle src, TextureHandle dst, Filter filter);
    void submit(bool wait);

    void bind_pipeline(PipelineHandle pipeline);
    void set_viewport(const Viewport& vp);
    void set_scissor(const Rect& r);
    void bind_vertex_buffer(std::uint32_t slot, BufferHandle buffer, std::uint32_t offset);
    void bind_index_buffer(BufferHandle buffer, IndexType type, std::uint32_t offset);
    void bind_texture(ShaderStage stage, std::uint32_t slot, TextureHandle texture, SamplerHandle sampler);
    void bind_storage_buffer(ShaderStage stage, std::uint32_t slot, BufferHandle buffer);
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance);
    void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                      std::int32_t vertex_offset, std::uint32_t first_instance);
    void draw_indexed_indirect(BufferHandle buffer, std::uint32_t offset, std::uint32_t draw_count);
    void end_render_pass();

    struct State;

private:
    // Several devices (and so contexts) may share a thread: every entry point that touches GL
    // outside a render pass re-binds this one when another is current.
    void make_current() const;
    explicit GlesBackend(Device& device);
    Device& device_;
    void* window_{nullptr};  // hidden context window (SDL_Window*)
    void* context_{nullptr}; // SDL_GLContext
    std::unique_ptr<State> s_;
};

} // namespace victus::rhi
