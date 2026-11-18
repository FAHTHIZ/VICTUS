// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/game_ui.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/ui/ui.hpp"

#include <algorithm>
#include <cfloat>
#include <span>

namespace victus::runtime {

namespace detail {
std::span<const unsigned char> inter_Medium() noexcept;
std::span<const unsigned char> inter_Bold() noexcept;
std::span<const unsigned char> inter_Medium_latin_ext() noexcept;
std::span<const unsigned char> inter_Bold_latin_ext() noexcept;
std::span<const unsigned char> inter_Medium_cyrillic() noexcept;
std::span<const unsigned char> inter_Bold_cyrillic() noexcept;
std::span<const unsigned char> inter_Medium_greek() noexcept;
std::span<const unsigned char> inter_Bold_greek() noexcept;
} // namespace detail

namespace {

constexpr float kBakeSize = 32.0f; // reference size; glyphs are baked at the size they are drawn

ImU32 to_imgui(math::Color c) {
    const auto u8 = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(u8(c.r), u8(c.g), u8(c.b), u8(c.a));
}

ImFont* add_font(ImGuiIO& io, std::span<const unsigned char> data, const char* name, bool merge = false) {
    ImFontConfig cfg;
    cfg.MergeMode = merge; // merged sources extend the previous font's glyph coverage
    cfg.FontDataOwnedByAtlas = false; // static or owned by the renderer
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
    // stb_truetype only reads the buffer; ImGui's signature is non-const for owned data.
    return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()), kBakeSize, &cfg);
}

// Splits `text` into lines (explicit newlines, plus word wrap at `wrap_width` when > 0).
template <class Fn>
void for_lines(ImFont* font, float size, std::string_view text, float wrap_width, Fn&& fn) {
    const char* p = text.data();
    const char* end = text.data() + text.size();
    while (p <= end) {
        const char* nl = std::find(p, end, '\n');
        const char* line = p;
        if (wrap_width > 0) {
            while (line < nl) {
                const char* brk = font->CalcWordWrapPosition(size, line, nl, wrap_width);
                if (brk <= line) brk = line + 1;
                const char* trimmed = brk;
                while (trimmed > line && trimmed[-1] == ' ') --trimmed;
                fn(line, trimmed);
                line = brk;
                while (line < nl && *line == ' ') ++line;
            }
            if (line == p) fn(p, nl); // empty line
        } else {
            fn(p, nl);
        }
        if (nl == end) break;
        p = nl + 1;
    }
}

} // namespace

Result<std::unique_ptr<GameUiRenderer>> GameUiRenderer::create(render::GpuContext& gpu, rhi::Format target_format) {
    std::unique_ptr<GameUiRenderer> r(new GameUiRenderer());
    r->gpu_ = &gpu;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ui::UiOptions o;
    o.font_size = kBakeSize;
    o.docking = false;
    o.victus_theme = false;
    auto ctx = ui::UiContext::create(gpu, target_format, nullptr, nullptr, o);
    if (!ctx) {
        ImGui::SetCurrentContext(previous);
        return std::move(ctx).error();
    }
    r->ui_ = std::move(ctx).value();
    ImGui::SetCurrentContext(r->ui_->imgui());
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    // Each weight: the Latin subset, then Latin Extended, Cyrillic and Greek merged into it so
    // localized text in those scripts renders without a project font.
    r->medium_ = add_font(io, detail::inter_Medium(), "Inter Medium");
    const bool medium_ext = r->medium_ && add_font(io, detail::inter_Medium_latin_ext(), "Inter Medium", true) &&
                            add_font(io, detail::inter_Medium_cyrillic(), "Inter Medium", true) &&
                            add_font(io, detail::inter_Medium_greek(), "Inter Medium", true);
    r->bold_ = add_font(io, detail::inter_Bold(), "Inter Bold");
    const bool bold_ext = r->bold_ && add_font(io, detail::inter_Bold_latin_ext(), "Inter Bold", true) &&
                          add_font(io, detail::inter_Bold_cyrillic(), "Inter Bold", true) &&
                          add_font(io, detail::inter_Bold_greek(), "Inter Bold", true);
    if (!medium_ext || !bold_ext) VICTUS_LOG_WARN("ui", "extended UI glyph sets failed to load; only Latin-1 text renders");
    ImGui::SetCurrentContext(previous);
    if (!r->medium_ || !r->bold_) return make_error(ErrorCode::DeviceError, "embedded UI fonts failed to load");
    return r;
}

GameUiRenderer::~GameUiRenderer() {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    const bool ours = ui_ && previous == ui_->imgui();
    ui_.reset();
    ImGui::SetCurrentContext(ours ? nullptr : previous);
}

ImFont* GameUiRenderer::font_for(const gameui::DrawItem& item, GpuAssetCache& assets, GameUiStats& stats) {
    ImFont* fallback = item.bold ? bold_ : medium_;
    if (item.font.empty()) return fallback;
    auto it = fonts_.find(item.font);
    if (it == fonts_.end()) {
        ProjectFont pf;
        if (const auto* db = assets.database()) {
            if (auto path = fs::resolve_under(db->assets_dir(), item.font)) {
                if (auto bytes = fs::read_binary(*path, 64ull << 20)) {
                    pf.data.assign(bytes->begin(), bytes->end());
                    pf.font = add_font(ImGui::GetIO(), pf.data, item.font.c_str());
                }
            }
        }
        if (!pf.font) VICTUS_LOG_WARN("ui", "font '{}' could not be loaded; using the built-in font", item.font);
        it = fonts_.emplace(item.font, std::move(pf)).first;
    }
    if (!it->second.font) {
        ++stats.missing;
        return fallback;
    }
    return it->second.font;
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

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ui_->new_frame(1.0f / 60.0f, size, 1.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
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
                const float h = d.border_width * 0.5f;
                dl->AddRect({p0.x + h, p0.y + h}, {p1.x - h, p1.y - h}, to_imgui(d.border_color), std::max(0.0f, radius - h),
                            d.border_width);
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
            ImFont* font = font_for(d, assets, stats);
            const float fs = d.font_size;
            struct Line {
                const char* b;
                const char* e;
                float w;
            };
            std::vector<Line> lines;
            for_lines(font, fs, d.text, d.wrap ? d.rect.w : 0.0f, [&](const char* b, const char* e) {
                lines.push_back({b, e, font->CalcTextSizeA(fs, FLT_MAX, 0.0f, b, e).x});
            });
            const float total = fs * static_cast<float>(lines.size());
            float y = d.rect.y;
            if (d.vertical_align == gameui::VerticalAlign::Middle) y += (d.rect.h - total) * 0.5f;
            else if (d.vertical_align == gameui::VerticalAlign::Bottom) y += d.rect.h - total;
            const bool shadow = d.shadow_color.a > 0 && (d.shadow_offset.x != 0 || d.shadow_offset.y != 0);
            for (const Line& l : lines) {
                float x = d.rect.x;
                if (d.align == gameui::TextAlign::Center) x += (d.rect.w - l.w) * 0.5f;
                else if (d.align == gameui::TextAlign::Right) x += d.rect.w - l.w;
                x = std::floor(x + 0.5f);
                const float ly = std::floor(y + 0.5f);
                if (l.e > l.b) {
                    if (shadow) dl->AddText(font, fs, {x + d.shadow_offset.x, ly + d.shadow_offset.y}, to_imgui(d.shadow_color), l.b, l.e);
                    dl->AddText(font, fs, {x, ly}, to_imgui(d.color), l.b, l.e);
                }
                y += fs;
            }
            break;
        }
        }
        dl->PopClipRect();
        ++stats.items;
    }
    Status st = ui_->render_to(cmd, target, false);
    stats.vertices = ui_->stats().vertices;
    stats.fonts = 2;
    for (const auto& [_, f] : fonts_) stats.fonts += f.font ? 1u : 0u;
    ImGui::SetCurrentContext(previous);
    if (!st.ok()) return std::move(st).error();
    return stats;
}

} // namespace victus::runtime
