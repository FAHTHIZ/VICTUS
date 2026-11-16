// SPDX-License-Identifier: Apache-2.0
// Frame render graph: passes declare the textures they read and write; compile() derives
// dependencies, culls passes that do not contribute to an output, computes transient
// lifetimes and aliases compatible transient textures onto shared physical textures.
#pragma once
#include "victus/core/result.hpp"
#include "victus/rhi/rhi.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace victus::render {

struct RGTexture {
    std::uint32_t id{~0u};
    [[nodiscard]] bool valid() const noexcept { return id != ~0u; }
};

class RenderGraph;
class TransientTexturePool;

struct PassContext {
    rhi::Device& device;
    rhi::CommandBuffer& cmd;
    rhi::RenderPass* pass; // null for non-raster passes
    std::uint32_t width{0}, height{0}; // attachment extent
    const RenderGraph& graph;
    [[nodiscard]] rhi::TextureHandle texture(RGTexture t) const;
};

class PassBuilder {
public:
    // Colour attachment. Clear starts a new version; Load depends on prior writers.
    void write_color(RGTexture t, rhi::LoadOp load = rhi::LoadOp::Clear, math::Color clear = {0, 0, 0, 1});
    void write_depth(RGTexture t, rhi::LoadOp load = rhi::LoadOp::Clear, float clear_depth = 1.0f);
    // Sampled read in shaders.
    void read(RGTexture t);
    // Pass has effects outside the graph (readback, presentation) and is never culled.
    void side_effect() noexcept;
    // Pass records commands itself (copies, blits) instead of a raster pass.
    void no_raster() noexcept;

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& g, std::uint32_t pass) : graph_(g), pass_(pass) {}
    RenderGraph& graph_;
    std::uint32_t pass_;
};

using PassExecute = std::function<void(PassContext&)>;

struct CompiledGraphInfo {
    std::vector<std::string> executed_passes; // in execution order
    std::uint32_t culled_passes{0};
    std::uint32_t transient_textures{0};
    std::uint32_t physical_textures{0}; // after aliasing
};

class RenderGraph {
public:
    RGTexture import_texture(std::string name, rhi::TextureHandle texture, const rhi::TextureDesc& desc);
    RGTexture create_texture(std::string name, rhi::TextureDesc desc);
    void add_pass(std::string name, const std::function<void(PassBuilder&)>& setup, PassExecute execute);
    // Imported/created textures whose final contents must be produced.
    void mark_output(RGTexture t);

    [[nodiscard]] Status compile();
    // Allocates transient textures from `pool` and records every surviving pass into `cmd`.
    [[nodiscard]] Status execute(rhi::Device& device, rhi::CommandBuffer& cmd, TransientTexturePool& pool);

    [[nodiscard]] const CompiledGraphInfo& info() const noexcept { return info_; }
    [[nodiscard]] const rhi::TextureDesc& desc(RGTexture t) const { return resources_.at(t.id).desc; }
    [[nodiscard]] rhi::TextureHandle physical(RGTexture t) const { return resources_.at(t.id).physical; }
    // Physical slot assignment after compile() (transients only, -1 for imported) for tests/tools.
    [[nodiscard]] int alias_slot(RGTexture t) const { return resources_.at(t.id).alias_slot; }

private:
    friend class PassBuilder;
    struct Resource {
        std::string name;
        rhi::TextureDesc desc;
        bool imported{false};
        bool output{false};
        rhi::TextureHandle physical;
        int alias_slot{-1};
        int first_use{-1}, last_use{-1};
    };
    struct Access {
        std::uint32_t resource;
        enum Kind : std::uint8_t { Read, Color, Depth } kind;
        rhi::LoadOp load{rhi::LoadOp::Clear};
        math::Color clear{};
        float clear_depth{1.0f};
    };
    struct Pass {
        std::string name;
        std::vector<Access> accesses;
        PassExecute execute;
        bool side_effect{false};
        bool raster{true};
        bool alive{false};
        std::vector<std::uint32_t> deps;
    };
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    CompiledGraphInfo info_;
    bool compiled_{false};
};

// Frame-to-frame cache of transient textures keyed by description. Textures unused for
// `max_idle_frames` frames are released.
class TransientTexturePool {
public:
    explicit TransientTexturePool(rhi::Device& device, std::uint32_t max_idle_frames = 3)
        : device_(device), max_idle_(max_idle_frames) {}
    ~TransientTexturePool();
    TransientTexturePool(const TransientTexturePool&) = delete;
    TransientTexturePool& operator=(const TransientTexturePool&) = delete;

    [[nodiscard]] Result<rhi::TextureHandle> acquire(const rhi::TextureDesc& desc);
    void end_frame();
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint64_t allocations() const noexcept { return allocations_; }

private:
    struct Entry {
        rhi::TextureDesc desc;
        rhi::TextureHandle texture;
        std::uint64_t last_used{0};
        bool in_use{false};
    };
    rhi::Device& device_;
    std::uint32_t max_idle_;
    std::uint64_t frame_{1};
    std::uint64_t allocations_{0};
    std::vector<Entry> entries_;
};

} // namespace victus::render
