// SPDX-License-Identifier: Apache-2.0
// VICTUS Render Hardware Interface. Engine renderers program against these types only.
// Backend: SDL_GPU (Vulkan / Metal / D3D12) — see docs/TECHNOLOGY_DECISIONS.md ADR-0003.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace victus::platform {
class Window;
}

namespace victus::rhi {

enum class Backend : std::uint8_t { Vulkan, Metal, D3D12, Unknown };
[[nodiscard]] const char* to_string(Backend b) noexcept;

enum class Format : std::uint8_t {
    Invalid,
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    BGRA8_SRGB,
    R8_UNORM,
    RG8_UNORM,
    RGBA16_FLOAT,
    R16_FLOAT,
    R32_FLOAT,
    RG16_FLOAT,
    RGB10A2_UNORM,
    R11G11B10_FLOAT,
    D16_UNORM,
    D24_UNORM_S8,
    D32_FLOAT,
    BC1_RGBA_SRGB,
    BC3_RGBA_SRGB,
    BC7_RGBA_SRGB,
    BC1_RGBA_UNORM,
    BC3_RGBA_UNORM,
    BC7_RGBA_UNORM,
    ASTC_4x4_UNORM,
    ASTC_4x4_SRGB,
    ASTC_6x6_UNORM,
    ASTC_6x6_SRGB,
};
[[nodiscard]] bool is_depth(Format f) noexcept;
[[nodiscard]] std::uint32_t bytes_per_pixel(Format f) noexcept; // 0 for block-compressed formats
// Block footprint of a format: 1x1 and bytes_per_pixel for uncompressed formats.
struct FormatBlock {
    std::uint32_t width{1}, height{1}, bytes{0};
};
[[nodiscard]] FormatBlock format_block(Format f) noexcept;
[[nodiscard]] bool is_compressed(Format f) noexcept;
// Bytes of one mip level of a width x height image (whole blocks for compressed formats).
[[nodiscard]] std::uint64_t mip_size_bytes(Format f, std::uint32_t width, std::uint32_t height) noexcept;

enum TextureUsage : std::uint32_t {
    TextureUsageSampled = 1u << 0,
    TextureUsageColorTarget = 1u << 1,
    TextureUsageDepthTarget = 1u << 2,
    TextureUsageStorageRead = 1u << 3,
    TextureUsageStorageWrite = 1u << 4,
};
enum BufferUsage : std::uint32_t {
    BufferUsageVertex = 1u << 0,
    BufferUsageIndex = 1u << 1,
    BufferUsageIndirect = 1u << 2,
    BufferUsageStorageRead = 1u << 3,
    BufferUsageStorageWrite = 1u << 4,
};

enum class TextureType : std::uint8_t { Tex2D, Tex2DArray, Cube, Tex3D };
enum class LoadOp : std::uint8_t { Load, Clear, DontCare };
enum class StoreOp : std::uint8_t { Store, DontCare };
enum class Primitive : std::uint8_t { TriangleList, TriangleStrip, LineList, LineStrip, PointList };
enum class CullMode : std::uint8_t { None, Front, Back };
enum class FrontFace : std::uint8_t { CounterClockwise, Clockwise };
enum class CompareOp : std::uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class BlendMode : std::uint8_t { Opaque, Alpha, PremultipliedAlpha, Additive, Multiply };
enum class Filter : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Repeat, ClampToEdge, MirroredRepeat };
enum class ShaderStage : std::uint8_t { Vertex, Fragment, Compute };
enum class IndexType : std::uint8_t { U16, U32 };
enum class VertexFormat : std::uint8_t { Float, Float2, Float3, Float4, UByte4Norm, UShort2Norm, Int4, UInt, UShort4 };
enum class PresentMode : std::uint8_t { VSync, Immediate, Mailbox };

// Generational handles: stale handles are detected rather than dereferenced.
template <typename Tag>
struct Handle {
    std::uint32_t index{0};
    std::uint32_t generation{0};
    [[nodiscard]] bool valid() const noexcept { return generation != 0; }
    explicit operator bool() const noexcept { return valid(); }
    friend bool operator==(Handle, Handle) = default;
};
using TextureHandle = Handle<struct TextureTag>;
using BufferHandle = Handle<struct BufferTag>;
using SamplerHandle = Handle<struct SamplerTag>;
using ShaderHandle = Handle<struct ShaderTag>;
using PipelineHandle = Handle<struct PipelineTag>;

struct TextureDesc {
    std::string name;
    TextureType type{TextureType::Tex2D};
    Format format{Format::RGBA8_UNORM};
    std::uint32_t width{1}, height{1}, depth_or_layers{1};
    std::uint32_t mip_levels{1};
    std::uint32_t usage{TextureUsageSampled};
    std::uint32_t sample_count{1};
};

struct BufferDesc {
    std::string name;
    std::uint32_t size{0};
    std::uint32_t usage{BufferUsageVertex};
};

struct SamplerDesc {
    Filter min_filter{Filter::Linear}, mag_filter{Filter::Linear}, mip_filter{Filter::Linear};
    AddressMode address_u{AddressMode::Repeat}, address_v{AddressMode::Repeat}, address_w{AddressMode::Repeat};
    float max_anisotropy{0.0f}; // 0 disables
    bool compare{false};        // shadow sampling
    CompareOp compare_op{CompareOp::LessEqual};
};

// One shader with every backend representation the device may need.
struct ShaderDesc {
    std::string name;
    ShaderStage stage{ShaderStage::Vertex};
    std::span<const std::uint8_t> spirv;
    std::string_view msl;
    std::span<const std::uint8_t> dxil;
    std::string entry{"main"};
    std::uint32_t samplers{0}, storage_textures{0}, storage_buffers{0}, uniform_buffers{0};
};

struct VertexAttribute {
    std::uint32_t location{0};
    std::uint32_t buffer_slot{0};
    VertexFormat format{VertexFormat::Float3};
    std::uint32_t offset{0};
};
struct VertexBufferLayout {
    std::uint32_t slot{0};
    std::uint32_t stride{0};
    bool per_instance{false};
};

struct ColorTargetDesc {
    Format format{Format::RGBA8_UNORM};
    BlendMode blend{BlendMode::Opaque};
};

struct PipelineDesc {
    std::string name;
    ShaderHandle vertex, fragment;
    std::vector<VertexBufferLayout> vertex_buffers;
    std::vector<VertexAttribute> attributes;
    Primitive primitive{Primitive::TriangleList};
    CullMode cull{CullMode::Back};
    FrontFace front_face{FrontFace::CounterClockwise};
    bool wireframe{false};
    float depth_bias_constant{0.0f}, depth_bias_slope{0.0f};
    bool depth_clamp{false};
    bool depth_test{false};
    bool depth_write{false};
    CompareOp depth_compare{CompareOp::LessEqual};
    std::vector<ColorTargetDesc> color_targets;
    Format depth_format{Format::Invalid};
    std::uint32_t sample_count{1};
};

struct ColorAttachment {
    TextureHandle texture;
    std::uint32_t mip{0}, layer{0};
    LoadOp load{LoadOp::Clear};
    StoreOp store{StoreOp::Store};
    math::Color clear{0, 0, 0, 1};
    TextureHandle resolve; // MSAA resolve target
};
struct DepthAttachment {
    TextureHandle texture;
    LoadOp load{LoadOp::Clear};
    StoreOp store{StoreOp::Store};
    float clear_depth{1.0f};
    std::uint32_t layer{0};
};
struct RenderPassDesc {
    std::vector<ColorAttachment> colors;
    DepthAttachment depth;
};

struct Viewport {
    float x{0}, y{0}, width{0}, height{0}, min_depth{0}, max_depth{1};
};
struct Rect {
    std::int32_t x{0}, y{0}, width{0}, height{0};
};

struct DeviceCapabilities {
    Backend backend{Backend::Unknown};
    std::string driver_name;
    bool bc_compression{false};
    bool astc_compression{false};
    bool anisotropy{false};
    bool depth_clamp{false};
    std::uint32_t max_texture_size{0};
    std::uint32_t max_msaa{1};
};

struct DeviceStats {
    std::uint32_t live_textures{0}, live_buffers{0}, live_samplers{0}, live_shaders{0}, live_pipelines{0};
    std::uint64_t texture_bytes{0}, buffer_bytes{0};
    std::uint64_t frames_submitted{0};
    std::uint64_t draw_calls{0}, uploads{0}, upload_bytes{0};
};

class Device;
class CommandBuffer;

class RenderPass {
public:
    void bind_pipeline(PipelineHandle pipeline);
    void set_viewport(const Viewport& vp);
    void set_scissor(const Rect& r);
    void bind_vertex_buffer(std::uint32_t slot, BufferHandle buffer, std::uint32_t offset = 0);
    void bind_index_buffer(BufferHandle buffer, IndexType type, std::uint32_t offset = 0);
    // Textures + samplers in slot order for the given stage.
    void bind_texture(ShaderStage stage, std::uint32_t slot, TextureHandle texture, SamplerHandle sampler);
    void bind_storage_buffer(ShaderStage stage, std::uint32_t slot, BufferHandle buffer);
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1, std::uint32_t first_vertex = 0,
              std::uint32_t first_instance = 0);
    void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count = 1, std::uint32_t first_index = 0,
                      std::int32_t vertex_offset = 0, std::uint32_t first_instance = 0);
    void end();
    [[nodiscard]] bool active() const noexcept { return pass_ != nullptr; }

private:
    friend class CommandBuffer;
    RenderPass(Device* d, void* pass) : device_(d), pass_(pass) {}
    Device* device_;
    void* pass_;
};

class CommandBuffer {
public:
    CommandBuffer(CommandBuffer&& o) noexcept : device_(o.device_), cmd_(o.cmd_) { o.cmd_ = nullptr; }
    CommandBuffer& operator=(CommandBuffer&&) = delete;
    CommandBuffer(const CommandBuffer&) = delete;
    ~CommandBuffer();

    // Uniform data is pushed per stage/slot and applies to subsequent draws (SDL push-uniform model).
    void push_uniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size);
    template <typename T>
    void push_uniforms(ShaderStage stage, std::uint32_t slot, const T& value) {
        push_uniforms(stage, slot, &value, static_cast<std::uint32_t>(sizeof(T)));
    }
    // Copies through a staging buffer; must be called outside render passes. `discard` declares that
    // the previous contents are no longer needed (per-frame dynamic data): if the GPU is still
    // reading the buffer from an earlier submission the backend renames it instead of stalling.
    void upload_buffer(BufferHandle buffer, std::uint32_t offset, std::span<const std::uint8_t> data,
                       bool discard = false);
    void upload_texture(TextureHandle texture, std::span<const std::uint8_t> pixels, std::uint32_t mip = 0,
                        std::uint32_t layer = 0);
    void generate_mipmaps(TextureHandle texture);
    // Swapchain texture for this frame (invalid handle when minimized / unavailable).
    [[nodiscard]] TextureHandle acquire_swapchain(platform::Window& window, std::uint32_t& width, std::uint32_t& height);
    [[nodiscard]] RenderPass begin_render_pass(const RenderPassDesc& desc);
    void blit(TextureHandle src, TextureHandle dst, Filter filter = Filter::Linear);
    void submit();
    void submit_and_wait();
    [[nodiscard]] void* native() const noexcept { return cmd_; }

private:
    friend class Device;
    friend class RenderPass;
    CommandBuffer(Device* d, void* cmd) : device_(d), cmd_(cmd) {}
    Device* device_;
    void* cmd_;
};

struct DeviceDesc {
    bool debug{false};
    bool headless{false};       // allow device creation without a window
    std::string preferred_driver; // "vulkan", "metal", "direct3d12" or empty for default
    bool low_power{false};
};

class Device {
public:
    [[nodiscard]] static Result<std::unique_ptr<Device>> create(const DeviceDesc& desc);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept { return caps_; }
    [[nodiscard]] const DeviceStats& stats() const noexcept { return stats_; }

    [[nodiscard]] Status attach_window(platform::Window& window, PresentMode mode = PresentMode::VSync);
    void detach_window(platform::Window& window);
    [[nodiscard]] Format swapchain_format(platform::Window& window) const;
    [[nodiscard]] bool supports_format(Format format, std::uint32_t usage) const;

    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc& desc);
    [[nodiscard]] Result<BufferHandle> create_buffer(const BufferDesc& desc);
    [[nodiscard]] Result<SamplerHandle> create_sampler(const SamplerDesc& desc);
    [[nodiscard]] Result<ShaderHandle> create_shader(const ShaderDesc& desc);
    [[nodiscard]] Result<PipelineHandle> create_pipeline(const PipelineDesc& desc);
    void destroy(TextureHandle h);
    void destroy(BufferHandle h);
    void destroy(SamplerHandle h);
    void destroy(ShaderHandle h);
    void destroy(PipelineHandle h);
    [[nodiscard]] const TextureDesc* describe(TextureHandle h) const;

    [[nodiscard]] CommandBuffer begin_commands();
    void wait_idle();
    // Synchronous readback of mip 0 / layer 0 (tests, screenshots, thumbnails).
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_texture(TextureHandle texture);

    [[nodiscard]] void* native() const noexcept { return device_; }
    // Native object lookups for engine backends (e.g. UI renderer integration).
    [[nodiscard]] void* native_texture(TextureHandle h) const;
    [[nodiscard]] void* native_sampler(SamplerHandle h) const;
    [[nodiscard]] void* native_buffer(BufferHandle h) const;
    [[nodiscard]] void* native_pipeline(PipelineHandle h) const;
    // Registers an externally owned texture (swapchain image) for the current frame.
    [[nodiscard]] TextureHandle wrap_transient_texture(void* native, std::uint32_t w, std::uint32_t h, Format f);

    struct Impl;

private:
    Device() = default;
    friend class CommandBuffer;
    friend class RenderPass;
    void* device_{nullptr};
    std::unique_ptr<Impl> impl_;
    DeviceCapabilities caps_;
    DeviceStats stats_;
};

} // namespace victus::rhi
