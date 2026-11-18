// SPDX-License-Identifier: Apache-2.0
// SDL_GPU implementation of the VICTUS RHI.
#include "victus/rhi/rhi.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/platform/platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace victus::rhi {

const char* to_string(Backend b) noexcept {
    switch (b) {
    case Backend::Vulkan: return "vulkan";
    case Backend::Metal: return "metal";
    case Backend::D3D12: return "direct3d12";
    default: return "unknown";
    }
}

bool is_depth(Format f) noexcept { return f == Format::D16_UNORM || f == Format::D24_UNORM_S8 || f == Format::D32_FLOAT; }

std::uint32_t bytes_per_pixel(Format f) noexcept {
    switch (f) {
    case Format::R8_UNORM: return 1;
    case Format::RG8_UNORM:
    case Format::R16_FLOAT:
    case Format::D16_UNORM: return 2;
    case Format::RGBA8_UNORM:
    case Format::RGBA8_SRGB:
    case Format::BGRA8_UNORM:
    case Format::BGRA8_SRGB:
    case Format::R32_FLOAT:
    case Format::RG16_FLOAT:
    case Format::RGB10A2_UNORM:
    case Format::R11G11B10_FLOAT:
    case Format::D24_UNORM_S8:
    case Format::D32_FLOAT: return 4;
    case Format::RGBA16_FLOAT: return 8;
    default: return 0;
    }
}

FormatBlock format_block(Format f) noexcept {
    switch (f) {
    case Format::BC1_RGBA_SRGB:
    case Format::BC1_RGBA_UNORM: return {4, 4, 8};
    case Format::BC3_RGBA_SRGB:
    case Format::BC3_RGBA_UNORM:
    case Format::BC7_RGBA_SRGB:
    case Format::BC7_RGBA_UNORM:
    case Format::ASTC_4x4_UNORM:
    case Format::ASTC_4x4_SRGB: return {4, 4, 16};
    case Format::ASTC_6x6_UNORM:
    case Format::ASTC_6x6_SRGB: return {6, 6, 16};
    default: return {1, 1, bytes_per_pixel(f)};
    }
}

bool is_compressed(Format f) noexcept { return format_block(f).width > 1; }

std::uint64_t mip_size_bytes(Format f, std::uint32_t width, std::uint32_t height) noexcept {
    const FormatBlock b = format_block(f);
    const std::uint64_t bx = (std::uint64_t{width} + b.width - 1) / b.width, by = (std::uint64_t{height} + b.height - 1) / b.height;
    return bx * by * b.bytes;
}

namespace {

SDL_GPUTextureFormat to_sdl(Format f) {
    switch (f) {
    case Format::RGBA8_UNORM: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::BGRA8_UNORM: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::R8_UNORM: return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case Format::RG8_UNORM: return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    case Format::RGBA16_FLOAT: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case Format::R16_FLOAT: return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
    case Format::R32_FLOAT: return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    case Format::RG16_FLOAT: return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    case Format::RGB10A2_UNORM: return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
    case Format::R11G11B10_FLOAT: return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
    case Format::D16_UNORM: return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    case Format::D24_UNORM_S8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case Format::D32_FLOAT: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case Format::BC1_RGBA_SRGB: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB;
    case Format::BC3_RGBA_SRGB: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB;
    case Format::BC7_RGBA_SRGB: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB;
    case Format::BC1_RGBA_UNORM: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case Format::BC3_RGBA_UNORM: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case Format::BC7_RGBA_UNORM: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case Format::ASTC_4x4_UNORM: return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
    case Format::ASTC_4x4_SRGB: return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM_SRGB;
    case Format::ASTC_6x6_UNORM: return SDL_GPU_TEXTUREFORMAT_ASTC_6x6_UNORM;
    case Format::ASTC_6x6_SRGB: return SDL_GPU_TEXTUREFORMAT_ASTC_6x6_UNORM_SRGB;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

Format from_sdl(SDL_GPUTextureFormat f) {
    switch (f) {
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM: return Format::RGBA8_UNORM;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB: return Format::RGBA8_SRGB;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM: return Format::BGRA8_UNORM;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB: return Format::BGRA8_SRGB;
    case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM: return Format::RGB10A2_UNORM;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: return Format::RGBA16_FLOAT;
    default: return Format::Invalid;
    }
}

SDL_GPUTextureUsageFlags to_sdl_usage(std::uint32_t u) {
    SDL_GPUTextureUsageFlags f = 0;
    if (u & TextureUsageSampled) f |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (u & TextureUsageColorTarget) f |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if (u & TextureUsageDepthTarget) f |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (u & TextureUsageStorageRead) f |= SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    if (u & TextureUsageStorageWrite) f |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    return f;
}

SDL_GPUSampleCount to_sdl_samples(std::uint32_t n) {
    switch (n) {
    case 2: return SDL_GPU_SAMPLECOUNT_2;
    case 4: return SDL_GPU_SAMPLECOUNT_4;
    case 8: return SDL_GPU_SAMPLECOUNT_8;
    default: return SDL_GPU_SAMPLECOUNT_1;
    }
}

SDL_GPUCompareOp to_sdl(CompareOp c) {
    switch (c) {
    case CompareOp::Never: return SDL_GPU_COMPAREOP_NEVER;
    case CompareOp::Less: return SDL_GPU_COMPAREOP_LESS;
    case CompareOp::Equal: return SDL_GPU_COMPAREOP_EQUAL;
    case CompareOp::LessEqual: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case CompareOp::Greater: return SDL_GPU_COMPAREOP_GREATER;
    case CompareOp::NotEqual: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case CompareOp::Always: return SDL_GPU_COMPAREOP_ALWAYS;
    }
    return SDL_GPU_COMPAREOP_ALWAYS;
}

SDL_GPUVertexElementFormat to_sdl(VertexFormat f) {
    switch (f) {
    case VertexFormat::Float: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    case VertexFormat::Float2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case VertexFormat::Float3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    case VertexFormat::Float4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    case VertexFormat::UByte4Norm: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    case VertexFormat::UShort2Norm: return SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM;
    case VertexFormat::Int4: return SDL_GPU_VERTEXELEMENTFORMAT_INT4;
    case VertexFormat::UInt: return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
    case VertexFormat::UShort4: return SDL_GPU_VERTEXELEMENTFORMAT_USHORT4;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
}

SDL_GPUColorTargetBlendState to_sdl(BlendMode m) {
    SDL_GPUColorTargetBlendState b{};
    b.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    b.enable_color_write_mask = true;
    b.color_blend_op = SDL_GPU_BLENDOP_ADD;
    b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    switch (m) {
    case BlendMode::Opaque: b.enable_blend = false; break;
    case BlendMode::Alpha:
        b.enable_blend = true;
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::PremultipliedAlpha:
        b.enable_blend = true;
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        b.enable_blend = true;
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case BlendMode::Multiply:
        b.enable_blend = true;
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
    return b;
}

SDL_GPUSamplerAddressMode to_sdl(AddressMode a) {
    switch (a) {
    case AddressMode::Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case AddressMode::ClampToEdge: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    case AddressMode::MirroredRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
}

template <typename T>
struct SlotMap {
    struct Entry {
        T value{};
        std::uint32_t generation{0};
        bool alive{false};
    };
    std::vector<Entry> entries;
    std::vector<std::uint32_t> free_list;
    std::uint32_t live{0};

    template <typename H>
    H insert(T value) {
        std::uint32_t idx;
        if (!free_list.empty()) {
            idx = free_list.back();
            free_list.pop_back();
        } else {
            idx = static_cast<std::uint32_t>(entries.size());
            entries.push_back({});
        }
        Entry& e = entries[idx];
        e.value = std::move(value);
        e.generation = e.generation + 1 == 0 ? 1 : e.generation + 1;
        e.alive = true;
        ++live;
        return H{idx, e.generation};
    }
    template <typename H>
    T* get(H h) {
        if (h.index >= entries.size()) return nullptr;
        Entry& e = entries[h.index];
        return (e.alive && e.generation == h.generation) ? &e.value : nullptr;
    }
    template <typename H>
    bool remove(H h, T& out) {
        T* v = get(h);
        if (!v) return false;
        out = std::move(*v);
        entries[h.index].alive = false;
        free_list.push_back(h.index);
        --live;
        return true;
    }
};

struct TextureEntry {
    SDL_GPUTexture* tex{nullptr};
    TextureDesc desc;
    bool external{false};
    std::uint64_t bytes{0};
};
struct BufferEntry {
    SDL_GPUBuffer* buf{nullptr};
    std::uint32_t size{0};
};
struct PipelineEntry {
    SDL_GPUGraphicsPipeline* pipe{nullptr};
};

std::uint64_t texture_bytes(const TextureDesc& d) {
    std::uint64_t total = 0;
    std::uint32_t w = d.width, h = d.height;
    for (std::uint32_t m = 0; m < d.mip_levels; ++m) {
        total += format_block(d.format).bytes ? mip_size_bytes(d.format, w, h) : std::uint64_t{w} * h;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    const std::uint64_t layers = d.type == TextureType::Cube ? 6 : d.depth_or_layers;
    return total * layers * d.sample_count;
}

} // namespace

struct Device::Impl {
    SlotMap<TextureEntry> textures;
    SlotMap<BufferEntry> buffers;
    SlotMap<SDL_GPUSampler*> samplers;
    SlotMap<SDL_GPUShader*> shaders;
    SlotMap<PipelineEntry> pipelines;
    SlotMap<SDL_GPUComputePipeline*> compute_pipelines;
    std::vector<TextureHandle> transient;
    SDL_GPUTransferBuffer* upload{nullptr};
    std::uint32_t upload_size{0};
    SDL_GPUShaderFormat formats{0};
    std::vector<SDL_Window*> windows;
};

Result<std::unique_ptr<Device>> Device::create(const DeviceDesc& desc) {
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;
    // Optional Vulkan features are relaxed for Android coverage. VICTUS shaders never write
    // gl_ClipDistance or draw indirect with a first instance, and no pipeline uses depth clamping by
    // default, so those are always off: Mali-G52-class drivers lack depthClamp and shaderClipDistance,
    // and SDL refuses such devices outright while the features are requested. Anisotropic filtering
    // is wanted when present, so creation is retried without it before giving up.
    const auto try_create = [&](bool anisotropy) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, desc.debug);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, desc.low_power);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, anisotropy);
        if (!desc.preferred_driver.empty())
            SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, desc.preferred_driver.c_str());
        SDL_GPUDevice* d = SDL_CreateGPUDeviceWithProperties(props);
        SDL_DestroyProperties(props);
        return d;
    };
    bool anisotropy = true;
    SDL_GPUDevice* dev = try_create(true);
    if (!dev) {
        const std::string first = SDL_GetError();
        anisotropy = false;
        dev = try_create(false);
        if (!dev) return make_error(ErrorCode::DeviceError, std::format("no compatible GPU device: {}", first));
        VICTUS_LOG_WARN("rhi", "GPU device created without anisotropic filtering ({})", first);
    }

    auto device = std::unique_ptr<Device>(new Device());
    device->device_ = dev;
    device->impl_ = std::make_unique<Impl>();
    device->impl_->formats = SDL_GetGPUShaderFormats(dev) & formats;
    const std::string driver = SDL_GetGPUDeviceDriver(dev);
    auto& caps = device->caps_;
    caps.driver_name = driver;
    caps.backend = driver == "vulkan" ? Backend::Vulkan : driver == "metal" ? Backend::Metal : driver == "direct3d12" ? Backend::D3D12 : Backend::Unknown;
    caps.bc_compression = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    caps.astc_compression = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM_SRGB, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    caps.anisotropy = anisotropy;
    caps.depth_clamp = false; // never requested (see above): pipelines asking for it clip instead
    caps.compute = true; // every SDL_GPU backend has compute and indirect draws
    caps.max_texture_size = 8192; // SDL_GPU guaranteed minimum on supported devices
    for (std::uint32_t s : {8u, 4u, 2u}) {
        if (SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, to_sdl_samples(s))) {
            caps.max_msaa = s;
            break;
        }
    }
    const SDL_PropertiesID info = SDL_GetGPUDeviceProperties(dev);
    const char* name = SDL_GetStringProperty(info, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
    const char* drv = SDL_GetStringProperty(info, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, nullptr);
    if (name) caps.driver_name = std::format("{} ({}{}{})", driver, name, drv ? ", " : "", drv ? drv : "");
    VICTUS_LOG_INFO("rhi", "GPU device: {} msaa<={} bc={} astc={}", caps.driver_name, caps.max_msaa, caps.bc_compression, caps.astc_compression);
    return device;
}

Device::~Device() {
    if (!device_) return;
    auto* dev = static_cast<SDL_GPUDevice*>(device_);
    SDL_WaitForGPUIdle(dev);
    for (auto& e : impl_->pipelines.entries)
        if (e.alive) SDL_ReleaseGPUGraphicsPipeline(dev, e.value.pipe);
    for (auto& e : impl_->compute_pipelines.entries)
        if (e.alive) SDL_ReleaseGPUComputePipeline(dev, e.value);
    for (auto& e : impl_->shaders.entries)
        if (e.alive) SDL_ReleaseGPUShader(dev, e.value);
    for (auto& e : impl_->samplers.entries)
        if (e.alive) SDL_ReleaseGPUSampler(dev, e.value);
    for (auto& e : impl_->buffers.entries)
        if (e.alive) SDL_ReleaseGPUBuffer(dev, e.value.buf);
    for (auto& e : impl_->textures.entries)
        if (e.alive && !e.value.external) SDL_ReleaseGPUTexture(dev, e.value.tex);
    if (impl_->upload) SDL_ReleaseGPUTransferBuffer(dev, impl_->upload);
    for (SDL_Window* w : impl_->windows) SDL_ReleaseWindowFromGPUDevice(dev, w);
    SDL_DestroyGPUDevice(dev);
}

Status Device::attach_window(platform::Window& window, PresentMode mode) {
    auto* dev = static_cast<SDL_GPUDevice*>(device_);
    auto* w = static_cast<SDL_Window*>(window.native());
    if (!SDL_ClaimWindowForGPUDevice(dev, w)) return make_error(ErrorCode::DeviceError, std::format("claim window failed: {}", SDL_GetError()));
    impl_->windows.push_back(w);
    SDL_GPUPresentMode pm = SDL_GPU_PRESENTMODE_VSYNC;
    if (mode == PresentMode::Immediate && SDL_WindowSupportsGPUPresentMode(dev, w, SDL_GPU_PRESENTMODE_IMMEDIATE)) pm = SDL_GPU_PRESENTMODE_IMMEDIATE;
    if (mode == PresentMode::Mailbox && SDL_WindowSupportsGPUPresentMode(dev, w, SDL_GPU_PRESENTMODE_MAILBOX)) pm = SDL_GPU_PRESENTMODE_MAILBOX;
    SDL_SetGPUSwapchainParameters(dev, w, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
    return {};
}

void Device::detach_window(platform::Window& window) {
    auto* w = static_cast<SDL_Window*>(window.native());
    auto it = std::find(impl_->windows.begin(), impl_->windows.end(), w);
    if (it == impl_->windows.end()) return;
    SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice*>(device_));
    SDL_ReleaseWindowFromGPUDevice(static_cast<SDL_GPUDevice*>(device_), w);
    impl_->windows.erase(it);
}

Format Device::swapchain_format(platform::Window& window) const {
    return from_sdl(SDL_GetGPUSwapchainTextureFormat(static_cast<SDL_GPUDevice*>(device_), static_cast<SDL_Window*>(window.native())));
}

bool Device::supports_format(Format format, std::uint32_t usage) const {
    return SDL_GPUTextureSupportsFormat(static_cast<SDL_GPUDevice*>(device_), to_sdl(format), SDL_GPU_TEXTURETYPE_2D, to_sdl_usage(usage));
}

Result<TextureHandle> Device::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) return make_error(ErrorCode::InvalidArgument, "texture dimensions must be non-zero");
    SDL_GPUTextureCreateInfo ci{};
    ci.type = desc.type == TextureType::Cube ? SDL_GPU_TEXTURETYPE_CUBE
              : desc.type == TextureType::Tex2DArray ? SDL_GPU_TEXTURETYPE_2D_ARRAY
              : desc.type == TextureType::Tex3D ? SDL_GPU_TEXTURETYPE_3D
                                                : SDL_GPU_TEXTURETYPE_2D;
    ci.format = to_sdl(desc.format);
    ci.usage = to_sdl_usage(desc.usage);
    ci.width = desc.width;
    ci.height = desc.height;
    ci.layer_count_or_depth = desc.type == TextureType::Cube ? 6 : desc.depth_or_layers;
    ci.num_levels = std::max(1u, desc.mip_levels);
    ci.sample_count = to_sdl_samples(desc.sample_count);
    if (ci.format == SDL_GPU_TEXTUREFORMAT_INVALID) return make_error(ErrorCode::Unsupported, "unsupported texture format");
    SDL_GPUTexture* t = SDL_CreateGPUTexture(static_cast<SDL_GPUDevice*>(device_), &ci);
    if (!t) return make_error(ErrorCode::DeviceError, std::format("create texture '{}' failed: {}", desc.name, SDL_GetError()));
    if (!desc.name.empty()) SDL_SetGPUTextureName(static_cast<SDL_GPUDevice*>(device_), t, desc.name.c_str());
    TextureEntry e{t, desc, false, texture_bytes(desc)};
    stats_.texture_bytes += e.bytes;
    auto h = impl_->textures.insert<TextureHandle>(std::move(e));
    stats_.live_textures = impl_->textures.live;
    return h;
}

TextureHandle Device::wrap_transient_texture(void* native, std::uint32_t w, std::uint32_t h, Format f) {
    TextureDesc d;
    d.name = "swapchain";
    d.width = w;
    d.height = h;
    d.format = f;
    d.usage = TextureUsageColorTarget;
    auto handle = impl_->textures.insert<TextureHandle>(TextureEntry{static_cast<SDL_GPUTexture*>(native), d, true, 0});
    impl_->transient.push_back(handle);
    return handle;
}

const TextureDesc* Device::describe(TextureHandle h) const {
    auto* e = impl_->textures.get(h);
    return e ? &e->desc : nullptr;
}

Result<BufferHandle> Device::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0) return make_error(ErrorCode::InvalidArgument, "buffer size must be non-zero");
    SDL_GPUBufferCreateInfo ci{};
    if (desc.usage & BufferUsageVertex) ci.usage |= SDL_GPU_BUFFERUSAGE_VERTEX;
    if (desc.usage & BufferUsageIndex) ci.usage |= SDL_GPU_BUFFERUSAGE_INDEX;
    if (desc.usage & BufferUsageIndirect) ci.usage |= SDL_GPU_BUFFERUSAGE_INDIRECT;
    if (desc.usage & BufferUsageStorageRead) ci.usage |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    if (desc.usage & BufferUsageStorageWrite) ci.usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    if (desc.usage & BufferUsageComputeRead) ci.usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    ci.size = desc.size;
    SDL_GPUBuffer* b = SDL_CreateGPUBuffer(static_cast<SDL_GPUDevice*>(device_), &ci);
    if (!b) return make_error(ErrorCode::DeviceError, std::format("create buffer '{}' failed: {}", desc.name, SDL_GetError()));
    if (!desc.name.empty()) SDL_SetGPUBufferName(static_cast<SDL_GPUDevice*>(device_), b, desc.name.c_str());
    stats_.buffer_bytes += desc.size;
    auto h = impl_->buffers.insert<BufferHandle>(BufferEntry{b, desc.size});
    stats_.live_buffers = impl_->buffers.live;
    return h;
}

Result<SamplerHandle> Device::create_sampler(const SamplerDesc& desc) {
    SDL_GPUSamplerCreateInfo ci{};
    ci.min_filter = desc.min_filter == Filter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mag_filter = desc.mag_filter == Filter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mipmap_mode = desc.mip_filter == Filter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = to_sdl(desc.address_u);
    ci.address_mode_v = to_sdl(desc.address_v);
    ci.address_mode_w = to_sdl(desc.address_w);
    ci.max_lod = 1000.0f;
    if (desc.max_anisotropy > 1.0f) {
        ci.enable_anisotropy = true;
        ci.max_anisotropy = desc.max_anisotropy;
    }
    if (desc.compare) {
        ci.enable_compare = true;
        ci.compare_op = to_sdl(desc.compare_op);
    }
    SDL_GPUSampler* s = SDL_CreateGPUSampler(static_cast<SDL_GPUDevice*>(device_), &ci);
    if (!s) return make_error(ErrorCode::DeviceError, std::format("create sampler failed: {}", SDL_GetError()));
    auto h = impl_->samplers.insert<SamplerHandle>(s);
    stats_.live_samplers = impl_->samplers.live;
    return h;
}

Result<ShaderHandle> Device::create_shader(const ShaderDesc& desc) {
    SDL_GPUShaderCreateInfo ci{};
    ci.stage = desc.stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    ci.num_samplers = desc.samplers;
    ci.num_storage_textures = desc.storage_textures;
    ci.num_storage_buffers = desc.storage_buffers;
    ci.num_uniform_buffers = desc.uniform_buffers;
    std::string entry = desc.entry;
    if ((impl_->formats & SDL_GPU_SHADERFORMAT_SPIRV) && !desc.spirv.empty()) {
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.code = desc.spirv.data();
        ci.code_size = desc.spirv.size();
    } else if ((impl_->formats & SDL_GPU_SHADERFORMAT_MSL) && !desc.msl.empty()) {
        ci.format = SDL_GPU_SHADERFORMAT_MSL;
        ci.code = reinterpret_cast<const Uint8*>(desc.msl.data());
        ci.code_size = desc.msl.size();
        if (entry == "main") entry = "main0"; // SPIRV-Cross renames main for MSL
    } else {
        return make_error(ErrorCode::Unsupported, std::format("shader '{}' has no bytecode for backend {}", desc.name, caps_.driver_name));
    }
    ci.entrypoint = entry.c_str();
    SDL_GPUShader* s = SDL_CreateGPUShader(static_cast<SDL_GPUDevice*>(device_), &ci);
    if (!s) return make_error(ErrorCode::DeviceError, std::format("create shader '{}' failed: {}", desc.name, SDL_GetError()));
    auto h = impl_->shaders.insert<ShaderHandle>(s);
    stats_.live_shaders = impl_->shaders.live;
    return h;
}

Result<PipelineHandle> Device::create_pipeline(const PipelineDesc& desc) {
    SDL_GPUShader** vs = impl_->shaders.get(desc.vertex);
    SDL_GPUShader** fs = impl_->shaders.get(desc.fragment);
    if (!vs || !fs) return make_error(ErrorCode::InvalidArgument, std::format("pipeline '{}' references invalid shaders", desc.name));
    std::vector<SDL_GPUVertexBufferDescription> vbs;
    for (const auto& vb : desc.vertex_buffers)
        vbs.push_back({vb.slot, vb.stride, vb.per_instance ? SDL_GPU_VERTEXINPUTRATE_INSTANCE : SDL_GPU_VERTEXINPUTRATE_VERTEX, 0});
    std::vector<SDL_GPUVertexAttribute> attrs;
    for (const auto& a : desc.attributes) attrs.push_back({a.location, a.buffer_slot, to_sdl(a.format), a.offset});
    std::vector<SDL_GPUColorTargetDescription> colors;
    for (const auto& c : desc.color_targets) colors.push_back({to_sdl(c.format), to_sdl(c.blend)});

    SDL_GPUGraphicsPipelineCreateInfo ci{};
    ci.vertex_shader = *vs;
    ci.fragment_shader = *fs;
    ci.vertex_input_state.vertex_buffer_descriptions = vbs.data();
    ci.vertex_input_state.num_vertex_buffers = static_cast<Uint32>(vbs.size());
    ci.vertex_input_state.vertex_attributes = attrs.data();
    ci.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(attrs.size());
    switch (desc.primitive) {
    case Primitive::TriangleList: ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;
    case Primitive::TriangleStrip: ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; break;
    case Primitive::LineList: ci.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST; break;
    case Primitive::LineStrip: ci.primitive_type = SDL_GPU_PRIMITIVETYPE_LINESTRIP; break;
    case Primitive::PointList: ci.primitive_type = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
    }
    ci.rasterizer_state.fill_mode = desc.wireframe ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = desc.cull == CullMode::None ? SDL_GPU_CULLMODE_NONE : desc.cull == CullMode::Front ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_BACK;
    ci.rasterizer_state.front_face = desc.front_face == FrontFace::CounterClockwise ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE : SDL_GPU_FRONTFACE_CLOCKWISE;
    if (desc.depth_bias_constant != 0.0f || desc.depth_bias_slope != 0.0f) {
        ci.rasterizer_state.enable_depth_bias = true;
        ci.rasterizer_state.depth_bias_constant_factor = desc.depth_bias_constant;
        ci.rasterizer_state.depth_bias_slope_factor = desc.depth_bias_slope;
    }
    ci.rasterizer_state.enable_depth_clip = !(desc.depth_clamp && caps_.depth_clamp);
    ci.multisample_state.sample_count = to_sdl_samples(desc.sample_count);
    ci.depth_stencil_state.enable_depth_test = desc.depth_test;
    ci.depth_stencil_state.enable_depth_write = desc.depth_write;
    ci.depth_stencil_state.compare_op = to_sdl(desc.depth_compare);
    ci.target_info.color_target_descriptions = colors.data();
    ci.target_info.num_color_targets = static_cast<Uint32>(colors.size());
    if (desc.depth_format != Format::Invalid) {
        ci.target_info.has_depth_stencil_target = true;
        ci.target_info.depth_stencil_format = to_sdl(desc.depth_format);
    }
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(static_cast<SDL_GPUDevice*>(device_), &ci);
    if (!p) return make_error(ErrorCode::DeviceError, std::format("create pipeline '{}' failed: {}", desc.name, SDL_GetError()));
    auto h = impl_->pipelines.insert<PipelineHandle>(PipelineEntry{p});
    stats_.live_pipelines = impl_->pipelines.live;
    return h;
}

void Device::destroy(TextureHandle h) {
    TextureEntry e;
    if (!impl_->textures.remove(h, e)) return;
    if (!e.external) SDL_ReleaseGPUTexture(static_cast<SDL_GPUDevice*>(device_), e.tex);
    stats_.texture_bytes -= e.bytes;
    stats_.live_textures = impl_->textures.live;
}
void Device::destroy(BufferHandle h) {
    BufferEntry e;
    if (!impl_->buffers.remove(h, e)) return;
    SDL_ReleaseGPUBuffer(static_cast<SDL_GPUDevice*>(device_), e.buf);
    stats_.buffer_bytes -= e.size;
    stats_.live_buffers = impl_->buffers.live;
}
void Device::destroy(SamplerHandle h) {
    SDL_GPUSampler* s = nullptr;
    if (!impl_->samplers.remove(h, s)) return;
    SDL_ReleaseGPUSampler(static_cast<SDL_GPUDevice*>(device_), s);
    stats_.live_samplers = impl_->samplers.live;
}
void Device::destroy(ShaderHandle h) {
    SDL_GPUShader* s = nullptr;
    if (!impl_->shaders.remove(h, s)) return;
    SDL_ReleaseGPUShader(static_cast<SDL_GPUDevice*>(device_), s);
    stats_.live_shaders = impl_->shaders.live;
}
Result<ComputePipelineHandle> Device::create_compute_pipeline(const ComputePipelineDesc& desc) {
    SDL_GPUComputePipelineCreateInfo ci{};
    std::string entry = desc.entry;
    if ((impl_->formats & SDL_GPU_SHADERFORMAT_SPIRV) && !desc.spirv.empty()) {
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.code = desc.spirv.data();
        ci.code_size = desc.spirv.size();
    } else if ((impl_->formats & SDL_GPU_SHADERFORMAT_MSL) && !desc.msl.empty()) {
        ci.format = SDL_GPU_SHADERFORMAT_MSL;
        ci.code = reinterpret_cast<const Uint8*>(desc.msl.data());
        ci.code_size = desc.msl.size();
        if (entry == "main") entry = "main0";
    } else {
        return make_error(ErrorCode::Unsupported,
                          std::format("compute shader '{}' has no bytecode for backend {}", desc.name, caps_.driver_name));
    }
    if (desc.threads_x == 0 || desc.threads_y == 0 || desc.threads_z == 0)
        return make_error(ErrorCode::InvalidArgument, std::format("compute shader '{}' has no workgroup size", desc.name));
    ci.entrypoint = entry.c_str();
    ci.num_samplers = desc.samplers;
    ci.num_readonly_storage_textures = desc.readonly_storage_textures;
    ci.num_readonly_storage_buffers = desc.readonly_storage_buffers;
    ci.num_readwrite_storage_textures = desc.readwrite_storage_textures;
    ci.num_readwrite_storage_buffers = desc.readwrite_storage_buffers;
    ci.num_uniform_buffers = desc.uniform_buffers;
    ci.threadcount_x = desc.threads_x;
    ci.threadcount_y = desc.threads_y;
    ci.threadcount_z = desc.threads_z;
    SDL_GPUComputePipeline* p = SDL_CreateGPUComputePipeline(static_cast<SDL_GPUDevice*>(device_), &ci);
    if (!p) return make_error(ErrorCode::DeviceError, std::format("create compute pipeline '{}' failed: {}", desc.name, SDL_GetError()));
    auto h = impl_->compute_pipelines.insert<ComputePipelineHandle>(p);
    stats_.live_compute_pipelines = impl_->compute_pipelines.live;
    return h;
}

void Device::destroy(ComputePipelineHandle h) {
    SDL_GPUComputePipeline* p = nullptr;
    if (!impl_->compute_pipelines.remove(h, p)) return;
    SDL_ReleaseGPUComputePipeline(static_cast<SDL_GPUDevice*>(device_), p);
    stats_.live_compute_pipelines = impl_->compute_pipelines.live;
}

void Device::destroy(PipelineHandle h) {
    PipelineEntry e;
    if (!impl_->pipelines.remove(h, e)) return;
    SDL_ReleaseGPUGraphicsPipeline(static_cast<SDL_GPUDevice*>(device_), e.pipe);
    stats_.live_pipelines = impl_->pipelines.live;
}

void* Device::native_texture(TextureHandle h) const {
    auto* e = impl_->textures.get(h);
    return e ? e->tex : nullptr;
}
void* Device::native_sampler(SamplerHandle h) const {
    auto* s = impl_->samplers.get(h);
    return s ? *s : nullptr;
}
void* Device::native_buffer(BufferHandle h) const {
    auto* e = impl_->buffers.get(h);
    return e ? e->buf : nullptr;
}
void* Device::native_pipeline(PipelineHandle h) const {
    auto* e = impl_->pipelines.get(h);
    return e ? e->pipe : nullptr;
}

CommandBuffer Device::begin_commands() {
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(static_cast<SDL_GPUDevice*>(device_));
    if (!cb) VICTUS_LOG_ERROR("rhi", "acquire command buffer failed: {}", SDL_GetError());
    return CommandBuffer(this, cb);
}

void Device::wait_idle() { SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice*>(device_)); }

Result<std::vector<std::uint8_t>> Device::read_texture(TextureHandle h) {
    auto* e = impl_->textures.get(h);
    if (!e) return make_error(ErrorCode::InvalidArgument, "invalid texture handle");
    const std::uint32_t bpp = bytes_per_pixel(e->desc.format);
    if (bpp == 0) return make_error(ErrorCode::Unsupported, "readback of compressed formats is unsupported");
    auto* dev = static_cast<SDL_GPUDevice*>(device_);
    const std::uint32_t size = e->desc.width * e->desc.height * bpp;
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    if (!tb) return make_error(ErrorCode::DeviceError, SDL_GetError());
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion region{};
    region.texture = e->tex;
    region.w = e->desc.width;
    region.h = e->desc.height;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(cp, &region, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);
    std::vector<std::uint8_t> out(size);
    void* mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (mapped) std::memcpy(out.data(), mapped, size);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    if (!mapped) return make_error(ErrorCode::DeviceError, "map download buffer failed");
    return out;
}

Result<std::vector<std::uint8_t>> Device::read_buffer(BufferHandle h, std::uint32_t offset, std::uint32_t size) {
    auto* e = impl_->buffers.get(h);
    if (!e) return make_error(ErrorCode::InvalidArgument, "invalid buffer handle");
    if (offset > e->size) return make_error(ErrorCode::InvalidArgument, "read_buffer offset out of range");
    if (size == 0) size = e->size - offset;
    if (size > e->size - offset) return make_error(ErrorCode::InvalidArgument, "read_buffer range out of bounds");
    auto* dev = static_cast<SDL_GPUDevice*>(device_);
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    if (!tb) return make_error(ErrorCode::DeviceError, SDL_GetError());
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUBufferRegion src{e->buf, offset, size};
    SDL_GPUTransferBufferLocation dst{tb, 0};
    SDL_DownloadFromGPUBuffer(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);
    std::vector<std::uint8_t> out(size);
    void* mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (mapped) std::memcpy(out.data(), mapped, size);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    if (!mapped) return make_error(ErrorCode::DeviceError, "map download buffer failed");
    return out;
}

// ---- CommandBuffer -------------------------------------------------------------------

CommandBuffer::~CommandBuffer() {
    if (cmd_) {
        VICTUS_LOG_WARN("rhi", "command buffer destroyed without submit; submitting to avoid a leak");
        submit();
    }
}

void CommandBuffer::push_uniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size) {
    auto* cb = static_cast<SDL_GPUCommandBuffer*>(cmd_);
    if (stage == ShaderStage::Vertex) SDL_PushGPUVertexUniformData(cb, slot, data, size);
    else if (stage == ShaderStage::Compute) SDL_PushGPUComputeUniformData(cb, slot, data, size);
    else SDL_PushGPUFragmentUniformData(cb, slot, data, size);
}

void CommandBuffer::upload_buffer(BufferHandle buffer, std::uint32_t offset, std::span<const std::uint8_t> data, bool discard) {
    if (data.empty()) return;
    auto& impl = *device_->impl_;
    auto* entry = impl.buffers.get(buffer);
    if (!entry || offset + data.size() > entry->size) {
        VICTUS_LOG_ERROR("rhi", "upload_buffer out of range or invalid handle");
        return;
    }
    auto* dev = static_cast<SDL_GPUDevice*>(device_->device_);
    const auto size = static_cast<std::uint32_t>(data.size());
    if (impl.upload_size < size) {
        if (impl.upload) SDL_ReleaseGPUTransferBuffer(dev, impl.upload);
        impl.upload_size = std::max<std::uint32_t>(size, std::max<std::uint32_t>(impl.upload_size * 2, 1u << 20));
        SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, impl.upload_size, 0};
        impl.upload = SDL_CreateGPUTransferBuffer(dev, &ti);
    }
    // cycle=true: SDL gives a fresh backing store if the previous one is still in flight.
    void* mapped = SDL_MapGPUTransferBuffer(dev, impl.upload, true);
    std::memcpy(mapped, data.data(), size);
    SDL_UnmapGPUTransferBuffer(dev, impl.upload);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer*>(cmd_));
    SDL_GPUTransferBufferLocation src{impl.upload, 0};
    SDL_GPUBufferRegion dst{entry->buf, offset, size};
    SDL_UploadToGPUBuffer(cp, &src, &dst, discard);
    SDL_EndGPUCopyPass(cp);
    device_->stats_.uploads++;
    device_->stats_.upload_bytes += size;
}

void CommandBuffer::upload_texture(TextureHandle texture, std::span<const std::uint8_t> pixels, std::uint32_t mip, std::uint32_t layer) {
    auto& impl = *device_->impl_;
    auto* entry = impl.textures.get(texture);
    if (!entry) {
        VICTUS_LOG_ERROR("rhi", "upload_texture: invalid handle");
        return;
    }
    auto* dev = static_cast<SDL_GPUDevice*>(device_->device_);
    const auto size = static_cast<std::uint32_t>(pixels.size());
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    void* mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    std::memcpy(mapped, pixels.data(), size);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer*>(cmd_));
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    SDL_GPUTextureRegion dst{};
    dst.texture = entry->tex;
    dst.mip_level = mip;
    dst.layer = layer;
    dst.w = std::max(1u, entry->desc.width >> mip);
    dst.h = std::max(1u, entry->desc.height >> mip);
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_ReleaseGPUTransferBuffer(dev, tb); // deferred by SDL until the command buffer completes
    device_->stats_.uploads++;
    device_->stats_.upload_bytes += size;
}

void CommandBuffer::generate_mipmaps(TextureHandle texture) {
    if (auto* e = device_->impl_->textures.get(texture); e && e->desc.mip_levels > 1)
        SDL_GenerateMipmapsForGPUTexture(static_cast<SDL_GPUCommandBuffer*>(cmd_), e->tex);
}

TextureHandle CommandBuffer::acquire_swapchain(platform::Window& window, std::uint32_t& width, std::uint32_t& height) {
    SDL_GPUTexture* tex = nullptr;
    width = height = 0;
    VICTUS_PROFILE_SCOPE("gpu.acquire_swapchain"); // waits while the GPU is behind (GPU-bound frames)
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(static_cast<SDL_GPUCommandBuffer*>(cmd_), static_cast<SDL_Window*>(window.native()), &tex, &width, &height) || !tex)
        return {};
    return device_->wrap_transient_texture(tex, width, height, device_->swapchain_format(window));
}

RenderPass CommandBuffer::begin_render_pass(const RenderPassDesc& desc) {
    auto& impl = *device_->impl_;
    SDL_GPUColorTargetInfo colors[4]{};
    const auto count = static_cast<Uint32>(std::min<std::size_t>(desc.colors.size(), 4));
    for (Uint32 i = 0; i < count; ++i) {
        const auto& c = desc.colors[i];
        auto* e = impl.textures.get(c.texture);
        if (!e) {
            VICTUS_LOG_ERROR("rhi", "render pass color attachment {} invalid", i);
            return RenderPass(device_, nullptr);
        }
        colors[i].texture = e->tex;
        colors[i].mip_level = c.mip;
        colors[i].layer_or_depth_plane = c.layer;
        colors[i].clear_color = {c.clear.r, c.clear.g, c.clear.b, c.clear.a};
        colors[i].load_op = c.load == LoadOp::Clear ? SDL_GPU_LOADOP_CLEAR : c.load == LoadOp::Load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
        colors[i].store_op = c.store == StoreOp::Store ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        if (auto* r = impl.textures.get(c.resolve)) {
            colors[i].resolve_texture = r->tex;
            colors[i].store_op = c.store == StoreOp::Store ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_RESOLVE;
        }
    }
    SDL_GPUDepthStencilTargetInfo depth{};
    SDL_GPUDepthStencilTargetInfo* depth_ptr = nullptr;
    if (auto* d = impl.textures.get(desc.depth.texture)) {
        depth.texture = d->tex;
        depth.clear_depth = desc.depth.clear_depth;
        depth.load_op = desc.depth.load == LoadOp::Clear ? SDL_GPU_LOADOP_CLEAR : desc.depth.load == LoadOp::Load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
        depth.store_op = desc.depth.store == StoreOp::Store ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth_ptr = &depth;
    }
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(static_cast<SDL_GPUCommandBuffer*>(cmd_), colors, count, depth_ptr);
    if (!pass) VICTUS_LOG_ERROR("rhi", "begin render pass failed: {}", SDL_GetError());
    return RenderPass(device_, pass);
}

ComputePass CommandBuffer::begin_compute_pass(std::span<const BufferHandle> readwrite_buffers) {
    auto& impl = *device_->impl_;
    std::array<SDL_GPUStorageBufferReadWriteBinding, 8> bindings{};
    const auto count = static_cast<Uint32>(std::min<std::size_t>(readwrite_buffers.size(), bindings.size()));
    for (Uint32 i = 0; i < count; ++i) {
        auto* e = impl.buffers.get(readwrite_buffers[i]);
        if (!e) {
            VICTUS_LOG_ERROR("rhi", "compute pass read-write buffer {} invalid", i);
            return ComputePass(device_, nullptr);
        }
        bindings[i].buffer = e->buf;
        bindings[i].cycle = false;
    }
    SDL_GPUComputePass* pass =
        SDL_BeginGPUComputePass(static_cast<SDL_GPUCommandBuffer*>(cmd_), nullptr, 0, count ? bindings.data() : nullptr, count);
    if (!pass) VICTUS_LOG_ERROR("rhi", "begin compute pass failed: {}", SDL_GetError());
    return ComputePass(device_, pass);
}

void CommandBuffer::blit(TextureHandle src, TextureHandle dst, Filter filter) {
    auto& impl = *device_->impl_;
    auto* s = impl.textures.get(src);
    auto* d = impl.textures.get(dst);
    if (!s || !d) return;
    SDL_GPUBlitInfo bi{};
    bi.source.texture = s->tex;
    bi.source.w = s->desc.width;
    bi.source.h = s->desc.height;
    bi.destination.texture = d->tex;
    bi.destination.w = d->desc.width;
    bi.destination.h = d->desc.height;
    bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
    bi.filter = filter == Filter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(static_cast<SDL_GPUCommandBuffer*>(cmd_), &bi);
}

void CommandBuffer::submit() {
    if (!cmd_) return;
    SDL_SubmitGPUCommandBuffer(static_cast<SDL_GPUCommandBuffer*>(cmd_));
    cmd_ = nullptr;
    device_->stats_.frames_submitted++;
    auto& impl = *device_->impl_;
    for (auto h : impl.transient) {
        TextureEntry e;
        impl.textures.remove(h, e);
    }
    impl.transient.clear();
}

void CommandBuffer::submit_and_wait() {
    if (!cmd_) return;
    VICTUS_PROFILE_SCOPE("gpu.submit_and_wait");
    auto* dev = static_cast<SDL_GPUDevice*>(device_->device_);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(static_cast<SDL_GPUCommandBuffer*>(cmd_));
    cmd_ = nullptr;
    if (fence) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    device_->stats_.frames_submitted++;
    auto& impl = *device_->impl_;
    for (auto h : impl.transient) {
        TextureEntry e;
        impl.textures.remove(h, e);
    }
    impl.transient.clear();
}

// ---- RenderPass ----------------------------------------------------------------------

void RenderPass::bind_pipeline(PipelineHandle pipeline) {
    if (auto* p = device_->impl_->pipelines.get(pipeline)) SDL_BindGPUGraphicsPipeline(static_cast<SDL_GPURenderPass*>(pass_), p->pipe);
}
void RenderPass::set_viewport(const Viewport& vp) {
    SDL_GPUViewport v{vp.x, vp.y, vp.width, vp.height, vp.min_depth, vp.max_depth};
    SDL_SetGPUViewport(static_cast<SDL_GPURenderPass*>(pass_), &v);
}
void RenderPass::set_scissor(const Rect& r) {
    SDL_Rect rect{r.x, r.y, r.width, r.height};
    SDL_SetGPUScissor(static_cast<SDL_GPURenderPass*>(pass_), &rect);
}
void RenderPass::bind_vertex_buffer(std::uint32_t slot, BufferHandle buffer, std::uint32_t offset) {
    if (auto* b = device_->impl_->buffers.get(buffer)) {
        SDL_GPUBufferBinding bind{b->buf, offset};
        SDL_BindGPUVertexBuffers(static_cast<SDL_GPURenderPass*>(pass_), slot, &bind, 1);
    }
}
void RenderPass::bind_index_buffer(BufferHandle buffer, IndexType type, std::uint32_t offset) {
    if (auto* b = device_->impl_->buffers.get(buffer)) {
        SDL_GPUBufferBinding bind{b->buf, offset};
        SDL_BindGPUIndexBuffer(static_cast<SDL_GPURenderPass*>(pass_), &bind, type == IndexType::U16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT : SDL_GPU_INDEXELEMENTSIZE_32BIT);
    }
}
void RenderPass::bind_texture(ShaderStage stage, std::uint32_t slot, TextureHandle texture, SamplerHandle sampler) {
    auto* t = device_->impl_->textures.get(texture);
    auto* s = device_->impl_->samplers.get(sampler);
    if (!t || !s) {
        VICTUS_LOG_ERROR("rhi", "bind_texture: invalid texture or sampler handle (slot {})", slot);
        return;
    }
    SDL_GPUTextureSamplerBinding b{t->tex, *s};
    if (stage == ShaderStage::Vertex) SDL_BindGPUVertexSamplers(static_cast<SDL_GPURenderPass*>(pass_), slot, &b, 1);
    else SDL_BindGPUFragmentSamplers(static_cast<SDL_GPURenderPass*>(pass_), slot, &b, 1);
}
void RenderPass::bind_storage_buffer(ShaderStage stage, std::uint32_t slot, BufferHandle buffer) {
    auto* b = device_->impl_->buffers.get(buffer);
    if (!b) return;
    SDL_GPUBuffer* buf = b->buf;
    if (stage == ShaderStage::Vertex) SDL_BindGPUVertexStorageBuffers(static_cast<SDL_GPURenderPass*>(pass_), slot, &buf, 1);
    else SDL_BindGPUFragmentStorageBuffers(static_cast<SDL_GPURenderPass*>(pass_), slot, &buf, 1);
}
void RenderPass::draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance) {
    SDL_DrawGPUPrimitives(static_cast<SDL_GPURenderPass*>(pass_), vertex_count, instance_count, first_vertex, first_instance);
    device_->stats_.draw_calls++;
}
void RenderPass::draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) {
    SDL_DrawGPUIndexedPrimitives(static_cast<SDL_GPURenderPass*>(pass_), index_count, instance_count, first_index, vertex_offset, first_instance);
    device_->stats_.draw_calls++;
}
void RenderPass::draw_indexed_indirect(BufferHandle buffer, std::uint32_t offset, std::uint32_t draw_count) {
    auto* b = device_->impl_->buffers.get(buffer);
    if (!b || draw_count == 0) return;
    SDL_DrawGPUIndexedPrimitivesIndirect(static_cast<SDL_GPURenderPass*>(pass_), b->buf, offset, draw_count);
    device_->stats_.draw_calls += draw_count;
    device_->stats_.indirect_draws += draw_count;
}
void RenderPass::end() {
    if (pass_) SDL_EndGPURenderPass(static_cast<SDL_GPURenderPass*>(pass_));
    pass_ = nullptr;
}

// ---- ComputePass ---------------------------------------------------------------------

void ComputePass::bind_pipeline(ComputePipelineHandle pipeline) {
    if (auto* p = device_->impl_->compute_pipelines.get(pipeline))
        SDL_BindGPUComputePipeline(static_cast<SDL_GPUComputePass*>(pass_), *p);
}
void ComputePass::bind_storage_buffer(std::uint32_t slot, BufferHandle buffer) {
    auto* b = device_->impl_->buffers.get(buffer);
    if (!b) return;
    SDL_GPUBuffer* buf = b->buf;
    SDL_BindGPUComputeStorageBuffers(static_cast<SDL_GPUComputePass*>(pass_), slot, &buf, 1);
}
void ComputePass::bind_texture(std::uint32_t slot, TextureHandle texture, SamplerHandle sampler) {
    auto* t = device_->impl_->textures.get(texture);
    auto* s = device_->impl_->samplers.get(sampler);
    if (!t || !s) return;
    SDL_GPUTextureSamplerBinding b{t->tex, *s};
    SDL_BindGPUComputeSamplers(static_cast<SDL_GPUComputePass*>(pass_), slot, &b, 1);
}
void ComputePass::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (x == 0 || y == 0 || z == 0) return;
    SDL_DispatchGPUCompute(static_cast<SDL_GPUComputePass*>(pass_), x, y, z);
    device_->stats_.dispatches++;
}
void ComputePass::end() {
    if (pass_) SDL_EndGPUComputePass(static_cast<SDL_GPUComputePass*>(pass_));
    pass_ = nullptr;
}

} // namespace victus::rhi
