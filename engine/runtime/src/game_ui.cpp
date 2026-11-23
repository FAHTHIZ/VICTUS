// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/game_ui.hpp"
#include "victus/runtime/builtin_fonts.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/ui/ui.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <span>

namespace victus::runtime {

namespace {

ImU32 to_imgui(math::Color c) {
    const auto u8 = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(u8(c.r), u8(c.g), u8(c.b), u8(c.a));
}

std::shared_ptr<text::Font> builtin(std::span<const unsigned char> data, const char* name) {
    // Embedded arrays live for the program, so the font borrows them.
    auto f = text::Font::create({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()}, name, false);
    if (!f) {
        VICTUS_LOG_WARN("ui", "built-in font {} failed to load: {}", name, f.error().message);
        return nullptr;
    }
    return std::move(f).value();
}

text::Direction to_text(gameui::TextDirection d) {
    switch (d) {
    case gameui::TextDirection::LeftToRight: return text::Direction::LeftToRight;
    case gameui::TextDirection::RightToLeft: return text::Direction::RightToLeft;
    default: return text::Direction::Auto;
    }
}

template <class T>
void append_bytes(std::string& key, const T& v) {
    key.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

} // namespace

Result<std::unique_ptr<GameUiRenderer>> GameUiRenderer::create(render::GpuContext& gpu, rhi::Format target_format) {
    std::unique_ptr<GameUiRenderer> r(new GameUiRenderer());
    r->gpu_ = &gpu;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ui::UiOptions o;
    o.docking = false;
    o.victus_theme = false;
    auto ctx = ui::UiContext::create(gpu, target_format, nullptr, nullptr, o);
    if (!ctx) {
        ImGui::SetCurrentContext(previous);
        return std::move(ctx).error();
    }
    r->ui_ = std::move(ctx).value();
    ImGui::SetCurrentContext(r->ui_->imgui());
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::SetCurrentContext(previous);
    // Each weight: Inter's Latin subset first (its metrics size the text), then Latin Extended,
    // Cyrillic and Greek, then Noto Sans Bengali and Arabic for those scripts.
    r->medium_.add(builtin(detail::inter_Medium(), "Inter Medium"));
    r->bold_.add(builtin(detail::inter_Bold(), "Inter Bold"));
    if (r->medium_.empty() || r->bold_.empty()) return make_error(ErrorCode::DeviceError, "embedded UI fonts failed to load");
    r->medium_.add(builtin(detail::inter_Medium_latin_ext(), "Inter Medium Latin Extended"));
    r->medium_.add(builtin(detail::inter_Medium_cyrillic(), "Inter Medium Cyrillic"));
    r->medium_.add(builtin(detail::inter_Medium_greek(), "Inter Medium Greek"));
    r->medium_.add(builtin(detail::noto_Bengali_Medium(), "Noto Sans Bengali Medium"));
    r->medium_.add(builtin(detail::noto_Arabic_Medium(), "Noto Sans Arabic Medium"));
    r->bold_.add(builtin(detail::inter_Bold_latin_ext(), "Inter Bold Latin Extended"));
    r->bold_.add(builtin(detail::inter_Bold_cyrillic(), "Inter Bold Cyrillic"));
    r->bold_.add(builtin(detail::inter_Bold_greek(), "Inter Bold Greek"));
    r->bold_.add(builtin(detail::noto_Bengali_Bold(), "Noto Sans Bengali Bold"));
    r->bold_.add(builtin(detail::noto_Arabic_Bold(), "Noto Sans Arabic Bold"));
    rhi::TextureDesc desc;
    desc.name = "game_ui.glyph_atlas";
    desc.format = rhi::Format::RGBA8_UNORM;
    desc.width = r->atlas_.width();
    desc.height = r->atlas_.height();
    auto tex = gpu.device().create_texture(desc);
    if (!tex) return std::move(tex).error();
    r->atlas_texture_ = *tex;
    return r;
}

GameUiRenderer::~GameUiRenderer() {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    const bool ours = ui_ && previous == ui_->imgui();
    ui_.reset();
    ImGui::SetCurrentContext(ours ? nullptr : previous);
    if (gpu_ && atlas_texture_.valid()) gpu_->device().destroy(atlas_texture_);
}

namespace {

// One closed loop of a rounded rectangle inset by `d` pixels (negative: outset). Every loop has the
// same point count, so consecutive loops join into quads.
void rounded_loop(std::vector<ImVec2>& out, ImVec2 p0, ImVec2 p1, float radius, float d, int segments) {
    out.clear();
    const float r = radius > 0 ? std::max(radius - d, 0.0f) : 0.0f;
    const ImVec2 c[4] = {{p1.x - radius, p0.y + radius}, {p1.x - radius, p1.y - radius},
                         {p0.x + radius, p1.y - radius}, {p0.x + radius, p0.y + radius}};
    // Corner order top-right, bottom-right, bottom-left, top-left; angles in screen space (y down).
    const float start[4] = {-1.5707964f, 0.0f, 1.5707964f, 3.1415927f};
    const ImVec2 sharp[4] = {{p1.x - d, p0.y + d}, {p1.x - d, p1.y - d}, {p0.x + d, p1.y - d}, {p0.x + d, p0.y + d}};
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i <= segments; ++i) {
            if (radius <= 0) {
                out.push_back(sharp[k]);
                continue;
            }
            const float a = start[k] + 1.5707964f * static_cast<float>(i) / static_cast<float>(segments);
            out.push_back({c[k].x + std::cos(a) * r, c[k].y + std::sin(a) * r});
        }
    }
}

// An anti-aliased border `thickness` pixels wide inside [p0, p1], drawn with the white texel at
// `uv`: four loops (outer fringe, outer edge, inner edge, inner fringe) joined by quads, the same
// geometry Dear ImGui used for untextured anti-aliased strokes.
void add_border(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, float thickness, ImU32 col, ImVec2 uv) {
    if (thickness <= 0 || p1.x - p0.x <= 0 || p1.y - p0.y <= 0) return;
    thickness = std::min(thickness, std::min(p1.x - p0.x, p1.y - p0.y) * 0.5f);
    const int segments = radius > 0 ? std::clamp(static_cast<int>(std::ceil(radius * 0.5f)), 2, 16) : 0;
    const float alpha = std::min(thickness, 1.0f);
    const float solid0 = thickness >= 1 ? 0.5f : thickness * 0.5f;
    const float solid1 = thickness >= 1 ? thickness - 0.5f : thickness * 0.5f;
    const float insets[4] = {solid0 - 1.0f, solid0, solid1, solid1 + 1.0f};
    const ImU32 a_col = (col & ~IM_COL32_A_MASK) |
                        (static_cast<ImU32>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha + 0.5f) << IM_COL32_A_SHIFT);
    const ImU32 clear = col & ~IM_COL32_A_MASK;
    const ImU32 cols[4] = {clear, a_col, a_col, clear};
    static thread_local std::vector<ImVec2> loop;
    rounded_loop(loop, p0, p1, radius, 0, segments);
    const int n = static_cast<int>(loop.size());
    dl->PrimReserve(3 * n * 6, 4 * n);
    const auto base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    for (int ring = 0; ring < 4; ++ring) {
        rounded_loop(loop, p0, p1, radius, insets[ring], segments);
        for (const ImVec2& p : loop) dl->PrimWriteVtx(p, uv, cols[ring]);
    }
    for (int ring = 0; ring < 3; ++ring) {
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const auto a = static_cast<ImDrawIdx>(base + ring * n + i), b = static_cast<ImDrawIdx>(base + ring * n + j);
            const auto c = static_cast<ImDrawIdx>(base + (ring + 1) * n + j), e = static_cast<ImDrawIdx>(base + (ring + 1) * n + i);
            dl->PrimWriteIdx(a);
            dl->PrimWriteIdx(b);
            dl->PrimWriteIdx(c);
            dl->PrimWriteIdx(a);
            dl->PrimWriteIdx(c);
            dl->PrimWriteIdx(e);
        }
    }
}

} // namespace

const text::FontStack& GameUiRenderer::stack_for(const gameui::DrawItem& item, bool bold, GameUiStats& stats) {
    const text::FontStack& fallback = bold ? bold_ : medium_;
    if (item.font.empty()) return fallback;
    auto it = fonts_.find(item.font);
    if (it == fonts_.end()) {
        ProjectFont pf;
        if (const auto* db = assets_ ? assets_->database() : nullptr) {
            if (auto path = fs::resolve_under(db->assets_dir(), item.font)) {
                if (auto bytes = fs::read_binary(*path, 64ull << 20)) {
                    auto font = text::Font::create({reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()}, item.font);
                    if (font) {
                        pf.stack.add(std::move(font).value());
                        for (const auto& f : medium_.faces()) pf.stack.add(f);
                    } else {
                        VICTUS_LOG_WARN("ui", "font '{}': {}", item.font, font.error().message);
                    }
                }
            }
        }
        if (pf.stack.empty()) VICTUS_LOG_WARN("ui", "font '{}' could not be loaded; using the built-in font", item.font);
        it = fonts_.emplace(item.font, std::move(pf)).first;
    }
    if (it->second.stack.empty()) {
        ++stats.missing;
        return fallback;
    }
    return it->second.stack;
}

const text::Layout& GameUiRenderer::layout_for(const gameui::DrawItem& d, GameUiStats& stats) {
    text::LayoutOptions opt;
    opt.size = d.font_size;
    opt.wrap_width = d.wrap ? d.rect.w : 0.0f;
    opt.direction = to_text(d.direction);
    std::string key;
    key.reserve(d.text.size() + 32);
    const text::FontStack& regular = stack_for(d, d.bold, stats);
    append_bytes(key, reinterpret_cast<std::uintptr_t>(&regular)); // font stack identity
    append_bytes(key, opt.size);
    append_bytes(key, opt.wrap_width);
    append_bytes(key, opt.direction);
    key.push_back(d.rich ? 'r' : 'p');
    key.push_back(d.bold ? 'b' : 'n');
    key += d.text;
    auto it = layouts_.find(key);
    if (it == layouts_.end()) {
        text::Layout layout;
        if (d.rich) {
            // One style per run: shaped per run, wrapped and bidi-reordered as one paragraph.
            std::string plain;
            std::vector<text::StyleSpan> spans;
            std::vector<const text::FontStack*> stacks;
            const bool custom = !d.font.empty();
            for (const gameui::TextRun& r : runs_) {
                const auto b = static_cast<std::uint32_t>(plain.size());
                plain += r.text;
                const auto style = static_cast<std::uint16_t>(std::min<std::size_t>(stacks.size(), 0xFFFE));
                stacks.push_back(custom ? &regular : (r.bold ? &bold_ : &medium_));
                spans.push_back({b, static_cast<std::uint32_t>(plain.size()), style});
            }
            if (stacks.empty()) stacks.push_back(&regular);
            layout = text::layout_text(stacks, plain, opt, spans);
        } else {
            layout = text::layout_text(regular, d.text, opt);
        }
        it = layouts_.emplace(std::move(key), CachedLayout{std::move(layout), frame_}).first;
    }
    it->second.frame = frame_;
    stats.missing_glyphs += it->second.layout.missing;
    return it->second.layout;
}

void GameUiRenderer::draw_text(ImDrawList* dl, const gameui::DrawItem& d, const text::Layout& L, std::span<const std::uint32_t> colors,
                               GameUiStats& stats) {
    if (L.lines.empty()) return;
    const float total = L.height();
    float top = d.rect.y;
    if (d.vertical_align == gameui::VerticalAlign::Middle) top += (d.rect.h - total) * 0.5f;
    else if (d.vertical_align == gameui::VerticalAlign::Bottom) top += d.rect.h - total;
    const bool field = d.scroll_to_caret;
    // Left edge of each line: alignment (text fields align to their paragraph's start side), then
    // caret scrolling keeps the caret of a field inside the rect.
    std::vector<float> xs(L.lines.size());
    for (std::size_t i = 0; i < L.lines.size(); ++i) {
        const text::Line& line = L.lines[i];
        gameui::TextAlign align = d.align;
        if (field) align = line.rtl ? gameui::TextAlign::Right : gameui::TextAlign::Left;
        float x = d.rect.x;
        if (align == gameui::TextAlign::Center) x += (d.rect.w - line.width) * 0.5f;
        else if (align == gameui::TextAlign::Right) x += d.rect.w - line.width;
        xs[i] = x;
    }
    std::size_t caret_line = 0;
    float caret_px = 0;
    if (d.caret >= 0) {
        caret_line = text::line_of(L, static_cast<std::uint32_t>(d.caret));
        caret_px = text::caret_x(L, caret_line, static_cast<std::uint32_t>(d.caret));
        if (field) {
            const float cx = xs[caret_line] + caret_px;
            float shift = 0;
            if (cx > d.rect.x + d.rect.w - 2.0f) shift = d.rect.x + d.rect.w - 2.0f - cx;
            else if (cx < d.rect.x) shift = d.rect.x - cx;
            for (float& x : xs) x += shift;
        }
    }
    const bool shadow = d.shadow_color.a > 0 && (d.shadow_offset.x != 0 || d.shadow_offset.y != 0);
    const ImU32 base = to_imgui(d.color);
    const auto pass = [&](float ox, float oy, const ImU32* override_color) {
        for (std::size_t i = 0; i < L.lines.size(); ++i) {
            const float x0 = std::floor(xs[i] + 0.5f);
            const float baseline = std::floor(top + L.line_height * static_cast<float>(i) + 0.5f) + std::round(L.ascent);
            for (const text::Glyph& g : L.lines[i].glyphs) {
                const text::AtlasGlyph* a = atlas_.glyph(*L.faces[g.face], g.id, L.em);
                if (!a || a->empty) continue;
                const ImU32 col = override_color ? *override_color : (g.style < colors.size() ? colors[g.style] : base);
                if ((col >> IM_COL32_A_SHIFT & 0xFF) == 0) continue;
                const float gx = std::floor(x0 + g.x + 0.5f) + ox, gy = std::floor(baseline + g.y + 0.5f) + oy;
                dl->PrimReserve(6, 4);
                dl->PrimRectUV({gx + a->x0, gy + a->y0}, {gx + a->x1, gy + a->y1}, {a->u0, a->v0}, {a->u1, a->v1}, col);
                ++stats.glyphs;
            }
        }
    };
    if (shadow) {
        const ImU32 sc = to_imgui(d.shadow_color);
        pass(d.shadow_offset.x, d.shadow_offset.y, &sc);
    }
    pass(0, 0, nullptr);
    if (d.caret >= 0) {
        const float fs = d.font_size;
        const float ly = std::floor(top + L.line_height * static_cast<float>(caret_line) + 0.5f);
        const float cx = std::floor(xs[caret_line] + caret_px + 0.5f), cw = std::max(1.0f, std::floor(fs * 0.07f));
        dl->AddRectFilled({cx, ly + fs * 0.1f}, {cx + cw, ly + fs * 0.95f}, to_imgui(math::Color{d.color.r, d.color.g, d.color.b, d.opacity}));
    }
}

void GameUiRenderer::upload_atlas(rhi::CommandBuffer& cmd, GameUiStats& stats) {
    // Coverage goes in alpha with white colour, the format the UI pipeline samples.
    const auto px = atlas_.pixels();
    atlas_rgba_.resize(px.size() * 4);
    for (std::size_t i = 0; i < px.size(); ++i) {
        atlas_rgba_[i * 4 + 0] = atlas_rgba_[i * 4 + 1] = atlas_rgba_[i * 4 + 2] = 255;
        atlas_rgba_[i * 4 + 3] = px[i];
    }
    cmd.upload_texture(atlas_texture_, atlas_rgba_);
    stats.atlas_uploads = 1;
}

Result<GameUiStats> GameUiRenderer::render(rhi::CommandBuffer& cmd, scene::Scene& scene, GpuAssetCache& assets,
                                           rhi::TextureHandle target, std::uint32_t width, std::uint32_t height) {
    VICTUS_PROFILE_SCOPE("runtime.game_ui");
    GameUiStats stats;
    if (width == 0 || height == 0) return stats;
    const math::Vec2 size{static_cast<float>(width), static_cast<float>(height)};
    gameui::set_screen_size(scene, size);
    gameui::layout(scene);
    gameui::build_draw_list(scene, items_);
    if (items_.empty()) return stats;
    assets_ = &assets;
    ++frame_;
    atlas_.begin_frame();
    // Layouts unused for two seconds (at 60 Hz) are dropped; a burst of unique strings is capped.
    if (frame_ % 120 == 0 || layouts_.size() > 4096) {
        const bool over = layouts_.size() > 4096;
        std::erase_if(layouts_, [&](const auto& kv) { return over || kv.second.frame + 120 < frame_; });
    }

    // Every glyph of the frame is requested before any vertex is written: a glyph that makes the
    // atlas grow rescales the texture coordinates of the glyphs already placed.
    // (Counters come from the drawing pass below, so this pass uses scratch stats.)
    GameUiStats scratch;
    for (const gameui::DrawItem& d : items_) {
        if (d.kind != gameui::DrawItem::Kind::Text) continue;
        const gameui::Rect clip = d.clip.intersect({0, 0, size.x, size.y});
        if (clip.w <= 0 || clip.h <= 0) continue;
        if (d.rich) runs_ = gameui::parse_rich_text(d.text, d.color, d.bold, d.opacity);
        const text::Layout& layout = layout_for(d, scratch);
        for (const text::Line& line : layout.lines)
            for (const text::Glyph& g : line.glyphs) (void)atlas_.glyph(*layout.faces[g.face], g.id, layout.em);
    }
    if (const auto* desc = gpu_->device().describe(atlas_texture_);
        !desc || desc->width != atlas_.width() || desc->height != atlas_.height()) {
        rhi::TextureDesc td;
        td.name = "game_ui.glyph_atlas";
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = atlas_.width();
        td.height = atlas_.height();
        auto tex = gpu_->device().create_texture(td);
        if (!tex) return std::move(tex).error();
        // Released textures stay alive until the GPU finishes with them (SDL GPU and GL semantics).
        if (atlas_texture_.valid()) gpu_->device().destroy(atlas_texture_);
        atlas_texture_ = *tex;
        (void)atlas_.take_dirty();
        upload_atlas(cmd, stats);
    }

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ui_->new_frame(1.0f / 60.0f, size, 1.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Rectangles use the atlas' opaque texel instead of Dear ImGui's font texture, and outlines are
    // tessellated instead of textured, so fills and text share one texture and batch together.
    // Fills, borders and text all use the glyph atlas texture (fills and borders its opaque
    // texel), so a HUD batches like it did with Dear ImGui's font texture. Rounded fills are
    // tessellated (anti-aliased by vertex alpha) instead of using Dear ImGui's baked corners.
    ImDrawListSharedData* shared = dl->_Data;
    const ImVec2 imgui_white = shared->TexUvWhitePixel;
    const ImDrawFlags imgui_flags = dl->Flags;
    const ImVec2 atlas_white{atlas_.white_u(), atlas_.white_v()};
    shared->TexUvWhitePixel = atlas_white;
    dl->Flags = imgui_flags & ~(ImDrawFlags_UseTexForRoundCorners | ImDrawFlags_AllowTexForRoundCorners_);
    dl->PushTexture(ImTextureRef(ui::UiContext::texture_id(atlas_texture_)));
    for (const gameui::DrawItem& d : items_) {
        const gameui::Rect clip = d.clip.intersect({0, 0, size.x, size.y});
        if (clip.w <= 0 || clip.h <= 0) continue;
        dl->PushClipRect({clip.x, clip.y}, {clip.x + clip.w, clip.y + clip.h}, false);
        const ImVec2 p0{d.rect.x, d.rect.y}, p1{d.rect.x + d.rect.w, d.rect.y + d.rect.h};
        const float radius = std::min(d.corner_radius, std::min(d.rect.w, d.rect.h) * 0.5f);
        switch (d.kind) {
        case gameui::DrawItem::Kind::Rect:
            if (d.color.a > 0) dl->AddRectFilled(p0, p1, to_imgui(d.color), radius);
            if (d.border_width > 0 && d.border_color.a > 0) {
                add_border(dl, p0, p1, radius, d.border_width, to_imgui(d.border_color), atlas_white);
            }
            break;
        case gameui::DrawItem::Kind::Image: {
            rhi::TextureHandle tex;
            if (!d.texture.is_nil()) {
                if (auto t = assets.texture(d.texture)) tex = *t;
                else ++stats.missing;
            }
            if (!tex) {
                dl->AddRectFilled(p0, p1, to_imgui(d.color), radius);
                break;
            }
            ImVec2 q0 = p0, q1 = p1;
            if (d.preserve_aspect) {
                if (const auto* desc = gpu_->device().describe(tex); desc && desc->width > 0 && desc->height > 0) {
                    const float ta = static_cast<float>(desc->width) / static_cast<float>(desc->height);
                    const float ra = d.rect.w / std::max(d.rect.h, 1e-3f);
                    if (ta > ra) {
                        const float h = d.rect.w / ta;
                        q0.y = d.rect.y + (d.rect.h - h) * 0.5f;
                        q1.y = q0.y + h;
                    } else {
                        const float w = d.rect.h * ta;
                        q0.x = d.rect.x + (d.rect.w - w) * 0.5f;
                        q1.x = q0.x + w;
                    }
                }
            }
            dl->AddImageRounded(ImTextureRef(ui::UiContext::texture_id(tex)), q0, q1, {0, 0}, {1, 1}, to_imgui(d.color), radius);
            break;
        }
        case gameui::DrawItem::Kind::Text: {
            std::vector<std::uint32_t> colors;
            if (d.rich) {
                runs_ = gameui::parse_rich_text(d.text, d.color, d.bold, d.opacity);
                colors.reserve(runs_.size());
                for (const gameui::TextRun& r : runs_) colors.push_back(to_imgui(r.color));
            }
            const text::Layout& layout = layout_for(d, stats);
            draw_text(dl, d, layout, colors, stats);
            break;
        }
        }
        dl->PopClipRect();
        ++stats.items;
    }
    dl->PopTexture();
    dl->Flags = imgui_flags;
    shared->TexUvWhitePixel = imgui_white;
    if (atlas_.take_dirty()) upload_atlas(cmd, stats);
    stats.atlas_width = atlas_.width();
    stats.atlas_height = atlas_.height();
    Status st = ui_->render_to(cmd, target, false);
    stats.vertices = ui_->stats().vertices;
    stats.atlas_glyphs = atlas_.stats().glyphs;
    stats.layouts = static_cast<std::uint32_t>(layouts_.size());
    stats.fonts = 2;
    for (const auto& [_, f] : fonts_) stats.fonts += f.stack.empty() ? 0u : 1u;
    ImGui::SetCurrentContext(previous);
    if (!st.ok()) return std::move(st).error();
    return stats;
}

} // namespace victus::runtime
