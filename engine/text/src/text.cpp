// SPDX-License-Identifier: Apache-2.0
// Text shaping (HarfBuzz), bidirectional layout (SheenBidi) and glyph rasterisation (ADR-0224).
#include "victus/text/text.hpp"

#include <hb-ot.h>
#include <hb.h>

#include <SheenBidi/SheenBidi.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
// Only stb_truetype's scanline rasteriser is used (stbtt_Rasterize on outlines that HarfBuzz has
// already parsed and bounds-checked); none of its font-file parsers ever see font bytes, which
// stb_truetype does not validate. STBTT_STATIC keeps this copy private to this file.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace victus::text {

struct Font::Impl {
    hb_blob_t* blob{nullptr};
    hb_face_t* face{nullptr};
    hb_font_t* font{nullptr};
    ~Impl() {
        if (font) hb_font_destroy(font);
        if (face) hb_face_destroy(face);
        if (blob) hb_blob_destroy(blob);
    }
};

namespace {

constexpr std::size_t kMaxFontBytes = 64u << 20;
constexpr std::size_t kMaxTextBytes = 1u << 20; // UI text; longer input is truncated

int read_i16(const char* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>((static_cast<unsigned char>(p[0]) << 8) |
                                                                static_cast<unsigned char>(p[1])));
}

hb_unicode_funcs_t* ucd() { return hb_unicode_funcs_get_default(); }

bool is_mark(char32_t cp) {
    switch (hb_unicode_general_category(ucd(), cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT: // ZWJ, ZWNJ and other joiners stay with their base
        return true;
    default:
        return (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF); // variation selectors
    }
}

bool is_space(char32_t cp) { return cp == ' ' || cp == '\t' || cp == 0x3000 || cp == 0x00A0; }

bool is_cjk(char32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FFFF);
}

struct Cp {
    char32_t cp;
    std::uint32_t off; // byte offset into the text
    std::uint32_t len; // bytes
};

// UTF-8 decoder: overlong forms, surrogates and truncated sequences become U+FFFD for one byte.
void decode(std::string_view s, std::vector<Cp>& out) {
    out.clear();
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned c = p[i];
        char32_t cp = 0xFFFD;
        std::uint32_t len = 1;
        auto cont = [&](std::size_t k) { return i + k < n && (p[i + k] & 0xC0) == 0x80; };
        if (c < 0x80) cp = c;
        else if (c >= 0xC2 && c <= 0xDF && cont(1)) {
            cp = ((c & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu);
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF && cont(1) && cont(2)) {
            const char32_t v = ((c & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu);
            if (v >= 0x800 && (v < 0xD800 || v > 0xDFFF)) {
                cp = v;
                len = 3;
            }
        } else if (c >= 0xF0 && c <= 0xF4 && cont(1) && cont(2) && cont(3)) {
            const char32_t v = ((c & 0x07u) << 18) | ((p[i + 1] & 0x3Fu) << 12) | ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu);
            if (v >= 0x10000 && v <= 0x10FFFF) {
                cp = v;
                len = 4;
            }
        }
        out.push_back({cp, static_cast<std::uint32_t>(i), len});
        i += len;
    }
}

struct Item {
    std::uint32_t begin, end; // byte range
    std::uint16_t face;       // index into Layout::faces
    std::uint16_t style;
    hb_script_t script;
    bool rtl;
};

struct Shaper {
    const std::vector<std::shared_ptr<Font>>& faces;
    std::string_view text;
    float em;
    hb_buffer_t* buf{hb_buffer_create()};
    ~Shaper() { hb_buffer_destroy(buf); }

    float scale(std::uint16_t face) const { return em / static_cast<float>(faces[face]->units_per_em()); }

    // Shapes [it.begin, it.end) with the whole text as context, appending glyphs at pen `x`.
    float shape(const Item& it, float x, std::vector<Glyph>* out) {
        hb_buffer_clear_contents(buf);
        hb_buffer_add_utf8(buf, text.data(), static_cast<int>(text.size()), it.begin, static_cast<int>(it.end - it.begin));
        hb_buffer_set_direction(buf, it.rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        if (it.script != HB_SCRIPT_INVALID && it.script != HB_SCRIPT_UNKNOWN) hb_buffer_set_script(buf, it.script);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(faces[it.face]->impl().font, buf, nullptr, 0);
        unsigned count = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
        const float s = scale(it.face);
        for (unsigned g = 0; g < count; ++g) {
            const float adv = static_cast<float>(pos[g].x_advance) * s;
            if (out) {
                Glyph gl;
                gl.id = info[g].codepoint;
                gl.face = it.face;
                gl.style = it.style;
                gl.cluster = info[g].cluster;
                gl.x = x + static_cast<float>(pos[g].x_offset) * s;
                gl.y = -static_cast<float>(pos[g].y_offset) * s;
                gl.advance = adv;
                out->push_back(gl);
            }
            x += adv;
        }
        return x;
    }
};

} // namespace

// --- Font ----------------------------------------------------------------------------------------

Result<std::shared_ptr<Font>> Font::create(std::span<const std::uint8_t> data, std::string name, bool copy) {
    if (data.size() < 12) return make_error(ErrorCode::ParseError, "font '" + name + "' is too small to be a font");
    if (data.size() > kMaxFontBytes) return make_error(ErrorCode::LimitExceeded, "font '" + name + "' is larger than 64 MiB");
    std::shared_ptr<Font> f(new Font());
    f->name_ = std::move(name);
    std::span<const std::uint8_t> bytes = data;
    if (copy) {
        f->owned_.assign(data.begin(), data.end());
        bytes = f->owned_;
    }
    f->impl_ = std::make_unique<Impl>();
    Impl& im = *f->impl_;
    im.blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data()), static_cast<unsigned>(bytes.size()),
                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    if (hb_face_count(im.blob) == 0) return make_error(ErrorCode::ParseError, "'" + f->name_ + "' is not a TrueType/OpenType font");
    im.face = hb_face_create(im.blob, 0);
    f->glyphs_ = hb_face_get_glyph_count(im.face);
    f->upem_ = static_cast<int>(hb_face_get_upem(im.face));
    if (f->glyphs_ == 0 || f->upem_ < 16 || f->upem_ > 16384)
        return make_error(ErrorCode::ParseError, "font '" + f->name_ + "' has no glyphs or an invalid units-per-em");
    // hhea ascent/descent: the metric stb_truetype (and so Dear ImGui) uses for "pixel height".
    hb_blob_t* hhea = hb_face_reference_table(im.face, HB_TAG('h', 'h', 'e', 'a'));
    unsigned len = 0;
    const char* h = hb_blob_get_data(hhea, &len);
    if (h && len >= 36) {
        f->ascent_ = read_i16(h + 4);
        f->descent_ = read_i16(h + 6);
    } else {
        f->ascent_ = f->upem_ * 4 / 5;
        f->descent_ = -f->upem_ / 5;
    }
    hb_blob_destroy(hhea);
    if (f->ascent_ - f->descent_ <= 0) {
        f->ascent_ = f->upem_ * 4 / 5;
        f->descent_ = -f->upem_ / 5;
    }
    im.font = hb_font_create(im.face);
    hb_font_set_scale(im.font, f->upem_, f->upem_); // positions in font units
    hb_font_make_immutable(im.font);
    return f;
}

Font::~Font() = default;

bool Font::has_glyph(char32_t cp) const noexcept {
    hb_codepoint_t g = 0;
    return hb_font_get_nominal_glyph(impl_->font, cp, &g) && g != 0;
}

std::uint16_t FontStack::face_for(char32_t cp) const noexcept {
    for (std::size_t i = 0; i < faces_.size(); ++i)
        if (faces_[i]->has_glyph(cp)) return static_cast<std::uint16_t>(i);
    return 0;
}

// --- Layout --------------------------------------------------------------------------------------

float Layout::width() const noexcept {
    float w = 0;
    for (const Line& l : lines) w = std::max(w, l.width);
    return w;
}

Layout layout_text(const FontStack& fonts, std::string_view text, const LayoutOptions& options) {
    const FontStack* stacks[] = {&fonts};
    return layout_text(stacks, text, options, {});
}

Layout layout_text(std::span<const FontStack* const> stacks, std::string_view text, const LayoutOptions& options,
                   std::span<const StyleSpan> spans) {
    Layout L;
    L.size = std::max(options.size, 0.0f);
    L.line_height = options.line_height > 0 ? options.line_height : L.size;
    if (stacks.empty() || !stacks[0] || stacks[0]->empty() || L.size <= 0) return L;
    if (text.size() > kMaxTextBytes) text = text.substr(0, kMaxTextBytes);
    // Faces of every style, concatenated; style s owns faces [first_face[s], first_face[s + 1]).
    const std::size_t nstyles = std::min<std::size_t>(stacks.size(), 0xFFFF);
    std::vector<std::uint16_t> first_face(nstyles + 1, 0);
    for (std::size_t s = 0; s < nstyles; ++s) {
        const FontStack* st = stacks[s] && !stacks[s]->empty() ? stacks[s] : stacks[0];
        for (const auto& f : st->faces())
            if (L.faces.size() < 0xFFFF) L.faces.push_back(f);
        first_face[s + 1] = static_cast<std::uint16_t>(L.faces.size());
    }
    const Font& primary = *L.faces[0];
    const float pixel_scale = L.size / static_cast<float>(primary.ascent() - primary.descent());
    L.em = pixel_scale * static_cast<float>(primary.units_per_em());
    L.ascent = static_cast<float>(primary.ascent()) * pixel_scale;

    std::vector<Cp> cps;
    decode(text, cps);
    // Byte offset -> character index.
    std::vector<std::uint32_t> cp_at(text.size() + 1, static_cast<std::uint32_t>(cps.size()));
    for (std::size_t i = 0; i < cps.size(); ++i)
        for (std::uint32_t b = 0; b < cps[i].len; ++b) cp_at[cps[i].off + b] = static_cast<std::uint32_t>(i);
    std::vector<std::uint16_t> style(cps.size(), 0);
    for (const StyleSpan& sp : spans) {
        if (sp.style >= nstyles) continue;
        const std::uint32_t e = std::min<std::uint32_t>(sp.end, static_cast<std::uint32_t>(text.size()));
        for (std::uint32_t b = sp.begin; b < e; ++b) style[cp_at[b]] = sp.style; // later spans win
    }
    // Face per character within its style's stack: keep the current face while it can draw the
    // character (so spaces and punctuation join their neighbours' run), marks and joiners follow
    // their base.
    std::vector<std::uint16_t> face(cps.size(), 0);
    const auto& faces = L.faces;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const char32_t c = cps[i].cp;
        const std::uint16_t lo = first_face[style[i]], hi = first_face[style[i] + 1];
        const bool same_style = i && style[i - 1] == style[i];
        const std::uint16_t prev = same_style ? face[i - 1] : lo;
        if (c == '\n' || c == '\r') {
            face[i] = prev;
            continue;
        }
        if (same_style && is_mark(c) && (faces[prev]->has_glyph(c) || c == 0x200C || c == 0x200D)) face[i] = prev;
        else if (same_style && faces[prev]->has_glyph(c) &&
                 (is_space(c) || hb_unicode_script(ucd(), c) == HB_SCRIPT_COMMON || hb_unicode_script(ucd(), c) == HB_SCRIPT_INHERITED))
            face[i] = prev;
        else {
            face[i] = lo;
            bool found = false;
            for (std::uint16_t k = lo; k < hi && !found; ++k)
                if (faces[k]->has_glyph(c)) {
                    face[i] = k;
                    found = true;
                }
            if (!found && !is_mark(c) && c >= 0x20 && !is_space(c)) ++L.missing;
        }
    }
    Shaper shaper{L.faces, text, L.em};
    const SBLevel base_level = options.direction == Direction::LeftToRight   ? SBLevel{0}
                               : options.direction == Direction::RightToLeft ? SBLevel{1}
                                                                             : SBLevel{SBLevelDefaultLTR};
    std::vector<hb_script_t> script(cps.size(), HB_SCRIPT_INVALID);

    std::size_t pb = 0;
    while (pb <= text.size()) {
        std::size_t pe = text.find('\n', pb);
        if (pe == std::string_view::npos) pe = text.size();
        std::size_t content_end = pe;
        if (content_end > pb && text[content_end - 1] == '\r') --content_end;
        const auto plen = static_cast<SBUInteger>(content_end - pb);
        if (plen == 0) {
            Line l;
            l.begin = l.end = static_cast<std::uint32_t>(pb);
            l.rtl = options.direction == Direction::RightToLeft;
            L.lines.push_back(std::move(l));
        } else {
            SBCodepointSequence seq{SBStringEncodingUTF8, text.data() + pb, plen};
            SBAlgorithmRef algo = SBAlgorithmCreate(&seq);
            SBParagraphRef para = algo ? SBAlgorithmCreateParagraph(algo, 0, plen, base_level) : nullptr;
            if (!para) {
                if (algo) SBAlgorithmRelease(algo);
                break; // allocation failure: stop rather than lay out garbage
            }
            const SBLevel* levels = SBParagraphGetLevelsPtr(para);
            const bool para_rtl = (SBParagraphGetBaseLevel(para) & 1) != 0;
            // Scripts (UAX #24, Common/Inherited resolved to their neighbours).
            SBScriptLocatorRef loc = SBScriptLocatorCreate();
            SBScriptLocatorLoadCodepoints(loc, &seq);
            while (SBScriptLocatorMoveNext(loc)) {
                const SBScriptAgent* a = SBScriptLocatorGetAgent(loc);
                const hb_script_t s = hb_script_from_iso15924_tag(SBScriptGetUnicodeTag(a->script));
                for (SBUInteger b = a->offset; b < a->offset + a->length && pb + b < text.size(); ++b) {
                    const std::uint32_t ci = cp_at[pb + b];
                    if (ci < script.size()) script[ci] = s;
                }
            }
            SBScriptLocatorRelease(loc);
            const std::uint32_t first = cp_at[pb], last = cp_at[content_end]; // character range [first, last)
            // Items within [b, e) (byte offsets): constant level, face and script.
            auto items_in = [&](std::uint32_t b, std::uint32_t e, std::vector<Item>& out) {
                out.clear();
                std::uint32_t i = cp_at[b];
                const std::uint32_t iend = e >= content_end ? last : cp_at[e];
                while (i < iend) {
                    const SBLevel lv = levels[cps[i].off - pb];
                    Item it{cps[i].off, 0, face[i], style[i], script[i], (lv & 1) != 0};
                    std::uint32_t j = i + 1;
                    while (j < iend && levels[cps[j].off - pb] == lv && face[j] == it.face && style[j] == it.style &&
                           script[j] == it.script)
                        ++j;
                    it.end = j < cps.size() ? cps[j].off : static_cast<std::uint32_t>(text.size());
                    it.end = std::min(it.end, e);
                    out.push_back(it);
                    i = j;
                }
            };
            // Advance per character from one logical shaping pass, for line breaking.
            std::vector<float> adv(last - first, 0.0f);
            std::vector<Item> items;
            if (options.wrap_width > 0) {
                items_in(static_cast<std::uint32_t>(pb), static_cast<std::uint32_t>(content_end), items);
                std::vector<Glyph> gs;
                for (const Item& it : items) {
                    gs.clear();
                    shaper.shape(it, 0, &gs);
                    for (const Glyph& g : gs) {
                        const std::uint32_t ci = cp_at[std::min<std::size_t>(g.cluster, text.size())];
                        if (ci >= first && ci < last) adv[ci - first] += g.advance;
                    }
                }
            }
            // Greedy line breaking over characters [first, last). A break is allowed before
            // character i after spaces, a zero-width space or a hyphen, and around CJK ideographs;
            // never before a combining mark.
            auto opportunity = [&](std::uint32_t i) {
                const char32_t prev = cps[i - 1].cp, cur = cps[i].cp;
                if (is_mark(cur)) return false;
                return (is_space(prev) && !is_space(cur)) || prev == 0x200B || (prev == '-' && !is_space(cur)) || is_cjk(prev) ||
                       is_cjk(cur);
            };
            std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges; // character ranges
            if (options.wrap_width <= 0) ranges.push_back({first, last});
            else {
                std::uint32_t start = first;
                while (start < last) {
                    float w = 0;
                    std::uint32_t i = start, brk = 0;
                    while (i < last) {
                        const float a = adv[i - first];
                        if (!is_space(cps[i].cp) && i > start && w + a > options.wrap_width) break;
                        w += a;
                        ++i;
                        if (i < last && opportunity(i)) brk = i;
                    }
                    std::uint32_t end = last;
                    if (i < last && brk > start) end = brk;
                    else if (i < last) {
                        end = i; // one word wider than the line: break between characters
                        while (end > start + 1 && is_mark(cps[end].cp)) --end;
                    }
                    ranges.push_back({start, end});
                    start = end;
                }
            }
            for (auto [cb, ce] : ranges) {
                // Trailing spaces hang outside the line.
                std::uint32_t vis_end = ce;
                while (vis_end > cb && is_space(cps[vis_end - 1].cp) && options.wrap_width > 0) --vis_end;
                Line line;
                line.rtl = para_rtl;
                line.begin = cps[cb].off;
                line.end = vis_end < cps.size() ? cps[vis_end].off : static_cast<std::uint32_t>(text.size());
                if (vis_end == last) line.end = static_cast<std::uint32_t>(content_end);
                if (line.end > line.begin) {
                    SBLineRef sl = SBParagraphCreateLine(para, line.begin - pb, line.end - line.begin);
                    if (sl) {
                        const SBRun* runs = SBLineGetRunsPtr(sl);
                        const SBUInteger nruns = SBLineGetRunCount(sl);
                        float x = 0;
                        for (SBUInteger r = 0; r < nruns; ++r) {
                            const auto rb = static_cast<std::uint32_t>(pb + runs[r].offset);
                            const auto re = static_cast<std::uint32_t>(rb + runs[r].length);
                            items_in(rb, re, items);
                            const bool rtl = (runs[r].level & 1) != 0;
                            if (rtl) std::reverse(items.begin(), items.end());
                            for (Item it : items) {
                                it.rtl = rtl;
                                x = shaper.shape(it, x, &line.glyphs);
                            }
                        }
                        line.width = x;
                        SBLineRelease(sl);
                    }
                }
                L.lines.push_back(std::move(line));
            }
            SBParagraphRelease(para);
            SBAlgorithmRelease(algo);
        }
        if (pe >= text.size()) break;
        pb = pe + 1;
    }
    return L;
}

std::size_t line_of(const Layout& layout, std::uint32_t offset) noexcept {
    if (layout.lines.empty()) return 0;
    for (std::size_t i = 0; i < layout.lines.size(); ++i) {
        const Line& l = layout.lines[i];
        const std::uint32_t next = i + 1 < layout.lines.size() ? layout.lines[i + 1].begin : std::numeric_limits<std::uint32_t>::max();
        if (offset >= l.begin && offset < next) return i;
    }
    return offset < layout.lines.front().begin ? 0 : layout.lines.size() - 1;
}

float caret_x(const Layout& layout, std::size_t line, std::uint32_t offset) noexcept {
    if (line >= layout.lines.size()) return 0;
    const Line& l = layout.lines[line];
    if (l.glyphs.empty()) return l.rtl ? l.width : 0.0f;
    // Cluster boxes in visual order.
    struct Box {
        std::uint32_t cluster;
        float left, right;
        bool rtl;
    };
    std::vector<Box> boxes;
    float pen = 0;
    for (std::size_t g = 0; g < l.glyphs.size(); ++g) {
        const Glyph& gl = l.glyphs[g];
        if (!boxes.empty() && boxes.back().cluster == gl.cluster) boxes.back().right = pen + gl.advance;
        else boxes.push_back({gl.cluster, pen, pen + gl.advance, false});
        pen += gl.advance;
    }
    // Direction of each box: clusters decreasing left to right mean right-to-left text.
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const bool next_lower = i + 1 < boxes.size() && boxes[i + 1].cluster < boxes[i].cluster;
        const bool prev_higher = i > 0 && boxes[i - 1].cluster > boxes[i].cluster;
        boxes[i].rtl = next_lower || prev_higher;
        if (boxes.size() == 1) boxes[i].rtl = l.rtl;
    }
    if (offset >= l.end) {
        // After the logically last cluster: its trailing edge.
        const Box* lastb = &boxes[0];
        for (const Box& b : boxes)
            if (b.cluster >= lastb->cluster) lastb = &b;
        return lastb->rtl ? lastb->left : lastb->right;
    }
    const Box* best = nullptr;
    for (const Box& b : boxes)
        if (b.cluster <= offset && (!best || b.cluster > best->cluster)) best = &b;
    if (!best) return l.rtl ? l.width : 0.0f;
    return best->rtl ? best->right : best->left;
}

// --- Glyph atlas ---------------------------------------------------------------------------------

namespace {

struct Outline {
    std::vector<stbtt_vertex> v;
    float minx{std::numeric_limits<float>::max()}, miny{std::numeric_limits<float>::max()};
    float maxx{std::numeric_limits<float>::lowest()}, maxy{std::numeric_limits<float>::lowest()};
    bool overflow{false};
    void add(unsigned char type, float x, float y, float cx = 0, float cy = 0, float cx1 = 0, float cy1 = 0) {
        if (v.size() >= 65536) {
            overflow = true;
            return;
        }
        auto q = [&](float f) {
            if (!(std::fabs(f) < 32000.0f)) overflow = true;
            return static_cast<stbtt_vertex_type>(std::clamp(f, -32000.0f, 32000.0f));
        };
        stbtt_vertex vert{};
        vert.type = type;
        vert.x = q(x);
        vert.y = q(y);
        vert.cx = q(cx);
        vert.cy = q(cy);
        vert.cx1 = q(cx1);
        vert.cy1 = q(cy1);
        v.push_back(vert);
        // Control points bound a Bezier curve, so including them gives a conservative box.
        grow(x, y);
        if (type == STBTT_vcurve || type == STBTT_vcubic) grow(cx, cy);
        if (type == STBTT_vcubic) grow(cx1, cy1);
    }
    void grow(float px, float py) {
        minx = std::min(minx, px);
        maxx = std::max(maxx, px);
        miny = std::min(miny, py);
        maxy = std::max(maxy, py);
    }
};

hb_draw_funcs_t* outline_funcs() {
    static hb_draw_funcs_t* funcs = [] {
        hb_draw_funcs_t* f = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(
            f, [](hb_draw_funcs_t*, void* d, hb_draw_state_t*, float x, float y, void*) { static_cast<Outline*>(d)->add(STBTT_vmove, x, y); },
            nullptr, nullptr);
        hb_draw_funcs_set_line_to_func(
            f, [](hb_draw_funcs_t*, void* d, hb_draw_state_t*, float x, float y, void*) { static_cast<Outline*>(d)->add(STBTT_vline, x, y); },
            nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(
            f,
            [](hb_draw_funcs_t*, void* d, hb_draw_state_t*, float cx, float cy, float x, float y, void*) {
                static_cast<Outline*>(d)->add(STBTT_vcurve, x, y, cx, cy);
            },
            nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(
            f,
            [](hb_draw_funcs_t*, void* d, hb_draw_state_t*, float c1x, float c1y, float c2x, float c2y, float x, float y, void*) {
                static_cast<Outline*>(d)->add(STBTT_vcubic, x, y, c1x, c1y, c2x, c2y);
            },
            nullptr, nullptr);
        hb_draw_funcs_make_immutable(f);
        return f;
    }();
    return funcs;
}

} // namespace

std::size_t GlyphAtlas::KeyHash::operator()(const Key& k) const noexcept {
    std::size_t h = std::hash<const void*>{}(k.font);
    h ^= (static_cast<std::size_t>(k.id) * 0x9E3779B97F4A7C15ull) + (h << 6) + (h >> 2);
    h ^= (static_cast<std::size_t>(k.size_q) * 0xBF58476D1CE4E5B9ull) + (h << 6) + (h >> 2);
    return h;
}

GlyphAtlas::GlyphAtlas(std::uint32_t width, std::uint32_t height, std::uint32_t max_width, std::uint32_t max_height)
    : width_(std::clamp<std::uint32_t>(width, 64, 8192)), height_(std::clamp<std::uint32_t>(height, 64, 8192)),
      max_width_(std::clamp<std::uint32_t>(max_width, width_, 8192)),
      max_height_(std::clamp<std::uint32_t>(max_height, height_, 8192)),
      pixels_(static_cast<std::size_t>(width_) * height_, 0) {
    clear();
}

void GlyphAtlas::clear() {
    std::fill(pixels_.begin(), pixels_.end(), std::uint8_t{0});
    // The opaque 3x3 block at (0,0) is never used by glyphs: the first shelf starts at x = 4 and
    // later shelves start below it.
    for (std::size_t y = 0; y < 3; ++y)
        for (std::size_t x = 0; x < 3; ++x) pixels_[y * width_ + x] = 255;
    glyphs_.clear();
    shelf_x_ = 4;
    shelf_y_ = 1;
    shelf_h_ = 0;
    dirty_ = true;
    stats_.glyphs = 0;
    stats_.occupancy = 0;
}

bool GlyphAtlas::begin_frame(float max_occupancy) {
    const bool at_max = width_ >= max_width_ && height_ >= max_height_;
    if (stats_.overflows == 0 && (!at_max || stats_.occupancy <= max_occupancy)) return false;
    const std::uint32_t resets = stats_.resets + 1;
    clear();
    stats_.resets = resets;
    stats_.overflows = 0;
    return true;
}

bool GlyphAtlas::grow() {
    const bool wider = width_ < max_width_ && (width_ <= height_ || height_ >= max_height_);
    const bool taller = !wider && height_ < max_height_;
    if (!wider && !taller) return false;
    const std::uint32_t nw = wider ? std::min(width_ * 2, max_width_) : width_;
    const std::uint32_t nh = taller ? std::min(height_ * 2, max_height_) : height_;
    std::vector<std::uint8_t> next(static_cast<std::size_t>(nw) * nh, 0);
    for (std::size_t y = 0; y < height_; ++y)
        std::copy_n(pixels_.data() + y * width_, width_, next.data() + y * nw);
    const float su = static_cast<float>(width_) / static_cast<float>(nw);
    const float sv = static_cast<float>(height_) / static_cast<float>(nh);
    for (auto& [key, g] : glyphs_) {
        g.u0 *= su;
        g.u1 *= su;
        g.v0 *= sv;
        g.v1 *= sv;
    }
    pixels_ = std::move(next);
    width_ = nw;
    height_ = nh;
    stats_.occupancy = static_cast<float>(shelf_y_ + shelf_h_) / static_cast<float>(height_);
    ++stats_.grows;
    dirty_ = true;
    return true;
}

bool GlyphAtlas::place(int w, int h, int& x, int& y) {
    for (;;) {
        const bool fits_shelf = shelf_x_ + w + 1 <= static_cast<int>(width_);
        const int sy = fits_shelf ? shelf_y_ : shelf_y_ + shelf_h_ + 1;
        if (w + 2 <= static_cast<int>(width_) && sy + h + 1 <= static_cast<int>(height_)) {
            if (!fits_shelf) {
                shelf_y_ = sy;
                shelf_x_ = 1;
                shelf_h_ = 0;
            }
            break;
        }
        if (!grow()) return false;
    }
    x = shelf_x_;
    y = shelf_y_;
    shelf_x_ += w + 1;
    shelf_h_ = std::max(shelf_h_, h);
    stats_.occupancy = static_cast<float>(shelf_y_ + shelf_h_) / static_cast<float>(height_);
    return true;
}

const AtlasGlyph* GlyphAtlas::glyph(const Font& font, std::uint32_t id, float em) {
    if (!(em > 0) || em > 2048.0f) return nullptr;
    const auto size_q = static_cast<std::uint32_t>(std::lround(em * 4.0f));
    const Key key{&font, id, size_q};
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return &it->second;
    const float scale = (static_cast<float>(size_q) / 4.0f) / static_cast<float>(font.units_per_em());
    Outline o;
    AtlasGlyph g;
    if (id < font.glyph_count() && hb_font_draw_glyph_or_fail(font.impl().font, id, outline_funcs(), &o) && !o.v.empty() && !o.overflow) {
        const int ix0 = static_cast<int>(std::floor(o.minx * scale));
        const int iy0 = static_cast<int>(std::floor(-o.maxy * scale));
        const int ix1 = static_cast<int>(std::ceil(o.maxx * scale));
        const int iy1 = static_cast<int>(std::ceil(-o.miny * scale));
        const int w = ix1 - ix0, h = iy1 - iy0;
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024) {
            int x = 0, y = 0;
            if (!place(w, h, x, y)) {
                ++stats_.overflows;
                return nullptr;
            }
            stbtt__bitmap bm{w, h, static_cast<int>(width_), pixels_.data() + static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)};
            stbtt_Rasterize(&bm, 0.35f, o.v.data(), static_cast<int>(o.v.size()), scale, scale, 0.0f, 0.0f, ix0, iy0, 1, nullptr);
            g.empty = false;
            g.x0 = static_cast<float>(ix0);
            g.y0 = static_cast<float>(iy0);
            g.x1 = static_cast<float>(ix1);
            g.y1 = static_cast<float>(iy1);
            g.u0 = static_cast<float>(x) / static_cast<float>(width_);
            g.v0 = static_cast<float>(y) / static_cast<float>(height_);
            g.u1 = static_cast<float>(x + w) / static_cast<float>(width_);
            g.v1 = static_cast<float>(y + h) / static_cast<float>(height_);
            dirty_ = true;
            ++stats_.rasterised;
        }
    }
    ++stats_.glyphs;
    return &glyphs_.emplace(key, g).first->second;
}

} // namespace victus::text
