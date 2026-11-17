// SPDX-License-Identifier: Apache-2.0
#include "victus/render/render_graph.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <format>

namespace victus::render {

namespace {
bool same_shape(const rhi::TextureDesc& a, const rhi::TextureDesc& b) {
    return a.type == b.type && a.format == b.format && a.width == b.width && a.height == b.height &&
           a.depth_or_layers == b.depth_or_layers && a.mip_levels == b.mip_levels && a.usage == b.usage &&
           a.sample_count == b.sample_count;
}
} // namespace

rhi::TextureHandle PassContext::texture(RGTexture t) const { return graph.physical(t); }

void PassBuilder::write_color(RGTexture t, rhi::LoadOp load, math::Color clear) {
    graph_.passes_[pass_].accesses.push_back({t.id, RenderGraph::Access::Color, load, clear, 1.0f});
}
void PassBuilder::write_depth(RGTexture t, rhi::LoadOp load, float clear_depth) {
    graph_.passes_[pass_].accesses.push_back({t.id, RenderGraph::Access::Depth, load, {}, clear_depth});
}
void PassBuilder::read(RGTexture t) {
    graph_.passes_[pass_].accesses.push_back({t.id, RenderGraph::Access::Read, rhi::LoadOp::Load, {}, 1.0f});
}
void PassBuilder::side_effect() noexcept { graph_.passes_[pass_].side_effect = true; }
void PassBuilder::no_raster() noexcept { graph_.passes_[pass_].raster = false; }

RGTexture RenderGraph::import_texture(std::string name, rhi::TextureHandle texture, const rhi::TextureDesc& desc) {
    Resource r;
    r.name = std::move(name);
    r.desc = desc;
    r.imported = true;
    r.physical = texture;
    resources_.push_back(std::move(r));
    compiled_ = false;
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

RGTexture RenderGraph::create_texture(std::string name, rhi::TextureDesc desc) {
    Resource r;
    r.name = std::move(name);
    desc.name = r.name;
    r.desc = std::move(desc);
    resources_.push_back(std::move(r));
    compiled_ = false;
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

void RenderGraph::add_pass(std::string name, const std::function<void(PassBuilder&)>& setup, PassExecute execute) {
    Pass p;
    p.name = std::move(name);
    p.execute = std::move(execute);
    passes_.push_back(std::move(p));
    PassBuilder b(*this, static_cast<std::uint32_t>(passes_.size() - 1));
    if (setup) setup(b);
    compiled_ = false;
}

void RenderGraph::mark_output(RGTexture t) {
    resources_.at(t.id).output = true;
    compiled_ = false;
}

Status RenderGraph::compile() {
    VICTUS_PROFILE_SCOPE("RenderGraph::compile");
    info_ = {};
    const std::size_t n = passes_.size();
    // Validate and derive dependencies: each access depends on the most recent prior writer of
    // that resource, except colour/depth writes that clear (they start a fresh version).
    std::vector<int> last_writer(resources_.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        Pass& p = passes_[i];
        p.deps.clear();
        p.alive = false;
        std::uint32_t color_targets = 0, depth_targets = 0;
        for (const Access& a : p.accesses) {
            if (a.resource >= resources_.size())
                return make_error(ErrorCode::InvalidArgument, std::format("pass '{}' uses an unknown texture", p.name));
            const Resource& r = resources_[a.resource];
            if (a.kind == Access::Color) {
                ++color_targets;
                if (rhi::is_depth(r.desc.format))
                    return make_error(ErrorCode::InvalidArgument,
                                      std::format("pass '{}' writes depth texture '{}' as colour", p.name, r.name));
            }
            if (a.kind == Access::Depth) {
                ++depth_targets;
                if (!rhi::is_depth(r.desc.format))
                    return make_error(ErrorCode::InvalidArgument,
                                      std::format("pass '{}' writes colour texture '{}' as depth", p.name, r.name));
            }
            const bool needs_previous = a.kind == Access::Read || a.load == rhi::LoadOp::Load;
            if (needs_previous) {
                if (last_writer[a.resource] >= 0) p.deps.push_back(static_cast<std::uint32_t>(last_writer[a.resource]));
                else if (!r.imported)
                    return make_error(ErrorCode::InvalidArgument,
                                      std::format("pass '{}' reads transient '{}' before it is written", p.name, r.name));
            }
        }
        if (depth_targets > 1)
            return make_error(ErrorCode::InvalidArgument, std::format("pass '{}' has multiple depth targets", p.name));
        if (p.raster && color_targets + depth_targets == 0)
            return make_error(ErrorCode::InvalidArgument,
                              std::format("raster pass '{}' has no attachments (use no_raster())", p.name));
        for (const Access& a : p.accesses)
            if (a.kind != Access::Read) last_writer[a.resource] = static_cast<int>(i);
    }

    // Cull: keep side-effect passes and final writers of outputs, then everything they depend on.
    std::vector<std::uint32_t> stack;
    for (std::size_t i = 0; i < n; ++i)
        if (passes_[i].side_effect) stack.push_back(static_cast<std::uint32_t>(i));
    for (std::size_t r = 0; r < resources_.size(); ++r)
        if (resources_[r].output && last_writer[r] >= 0) stack.push_back(static_cast<std::uint32_t>(last_writer[r]));
    while (!stack.empty()) {
        std::uint32_t i = stack.back();
        stack.pop_back();
        if (passes_[i].alive) continue;
        passes_[i].alive = true;
        for (auto d : passes_[i].deps) stack.push_back(d);
    }

    // Lifetimes over surviving passes (declaration order is a valid topological order).
    for (auto& r : resources_) {
        r.first_use = r.last_use = -1;
        r.alias_slot = -1;
        if (!r.imported) r.physical = {};
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!passes_[i].alive) {
            ++info_.culled_passes;
            continue;
        }
        info_.executed_passes.push_back(passes_[i].name);
        for (const Access& a : passes_[i].accesses) {
            Resource& r = resources_[a.resource];
            if (r.first_use < 0) r.first_use = static_cast<int>(i);
            r.last_use = static_cast<int>(i);
        }
    }

    // Greedy aliasing: a transient reuses a slot whose previous occupant's lifetime ended and
    // whose description matches. Outputs are never aliased.
    struct Slot {
        rhi::TextureDesc desc;
        int busy_until;
    };
    std::vector<Slot> slots;
    std::vector<std::uint32_t> order;
    for (std::uint32_t r = 0; r < resources_.size(); ++r)
        if (!resources_[r].imported && resources_[r].first_use >= 0) order.push_back(r);
    std::sort(order.begin(), order.end(),
              [&](std::uint32_t a, std::uint32_t b) { return resources_[a].first_use < resources_[b].first_use; });
    for (auto r : order) {
        Resource& res = resources_[r];
        ++info_.transient_textures;
        int chosen = -1;
        if (!res.output) {
            for (std::size_t s = 0; s < slots.size(); ++s) {
                if (slots[s].busy_until < res.first_use && same_shape(slots[s].desc, res.desc)) {
                    chosen = static_cast<int>(s);
                    break;
                }
            }
        }
        if (chosen < 0) {
            slots.push_back({res.desc, res.output ? 1 << 30 : res.last_use});
            chosen = static_cast<int>(slots.size() - 1);
        } else {
            slots[static_cast<std::size_t>(chosen)].busy_until = res.last_use;
        }
        res.alias_slot = chosen;
    }
    info_.physical_textures = static_cast<std::uint32_t>(slots.size());
    compiled_ = true;
    return {};
}

Status RenderGraph::execute(rhi::Device& device, rhi::CommandBuffer& cmd, TransientTexturePool& pool) {
    VICTUS_PROFILE_SCOPE("RenderGraph::execute");
    if (!compiled_) VICTUS_TRY_STATUS(compile());
    // One physical texture per alias slot.
    std::vector<rhi::TextureHandle> slot_textures(info_.physical_textures);
    for (auto& r : resources_) {
        if (r.imported || r.alias_slot < 0) continue;
        auto& tex = slot_textures[static_cast<std::size_t>(r.alias_slot)];
        if (!tex) {
            auto acquired = pool.acquire(r.desc);
            if (!acquired) return std::move(acquired).error();
            tex = acquired.value();
        }
        r.physical = tex;
    }
    for (Pass& p : passes_) {
        if (!p.alive) continue;
        profiler::ScopedZone pass_zone(profiler::capturing() ? profiler::intern("pass." + p.name) : "RenderGraph::pass");
        std::uint32_t w = 0, h = 0;
        if (p.raster) {
            rhi::RenderPassDesc rp;
            for (const Access& a : p.accesses) {
                const Resource& r = resources_[a.resource];
                if (a.kind == Access::Color) {
                    rhi::ColorAttachment c;
                    c.texture = r.physical;
                    c.load = a.load;
                    c.clear = a.clear;
                    rp.colors.push_back(c);
                } else if (a.kind == Access::Depth) {
                    rp.depth.texture = r.physical;
                    rp.depth.load = a.load;
                    rp.depth.clear_depth = a.clear_depth;
                } else {
                    continue;
                }
                w = r.desc.width;
                h = r.desc.height;
            }
            rhi::RenderPass pass = cmd.begin_render_pass(rp);
            if (!pass.active())
                return make_error(ErrorCode::DeviceError, std::format("failed to begin render pass '{}'", p.name));
            pass.set_viewport({0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1});
            PassContext ctx{device, cmd, &pass, w, h, *this};
            if (p.execute) p.execute(ctx);
            pass.end();
        } else {
            PassContext ctx{device, cmd, nullptr, 0, 0, *this};
            if (p.execute) p.execute(ctx);
        }
    }
    pool.end_frame();
    return {};
}

TransientTexturePool::~TransientTexturePool() { clear(); }

Result<rhi::TextureHandle> TransientTexturePool::acquire(const rhi::TextureDesc& desc) {
    for (auto& e : entries_) {
        if (!e.in_use && same_shape(e.desc, desc)) {
            e.in_use = true;
            e.last_used = frame_;
            return e.texture;
        }
    }
    auto tex = device_.create_texture(desc);
    if (!tex) return std::move(tex).error();
    ++allocations_;
    entries_.push_back({desc, tex.value(), frame_, true});
    return tex.value();
}

void TransientTexturePool::end_frame() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        it->in_use = false;
        if (frame_ - it->last_used > max_idle_) {
            device_.destroy(it->texture);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    ++frame_;
}

void TransientTexturePool::clear() {
    for (auto& e : entries_) device_.destroy(e.texture);
    entries_.clear();
}

} // namespace victus::render
