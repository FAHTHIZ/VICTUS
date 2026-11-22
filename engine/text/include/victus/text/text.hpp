// SPDX-License-Identifier: Apache-2.0
// VICTUS text: OpenType shaping and bidirectional line layout for UI text (ADR-0224).
//
// - `Font` wraps one TrueType/OpenType face (HarfBuzz for shaping, stb_truetype for glyph
//   coverage). Font bytes are untrusted: they are validated and rejected with an error, never
//   trusted to be well formed.
// - `FontStack` is a primary face followed by fallbacks. Each character uses the first face that
//   has a glyph for it (marks and joiners stay with their base), so Latin text in Inter and
//   Bengali or Arabic in Noto mix in one string.
// - `layout_text` splits paragraphs at '\n', resolves embedding levels with the Unicode
//   Bidirectional Algorithm (SheenBidi), wraps at word boundaries, reorders each line visually
//   and shapes every run (HarfBuzz) in its direction and script. The result is glyph ids with pen
//   positions in pixels, ready to draw.
// - `GlyphAtlas` rasterises glyphs by id (not by code point, which shaped text needs) into one
//   8-bit coverage texture.
#pragma once

#include "victus/core/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace victus::text {

enum class Direction : std::uint8_t {
    Auto,        // from the first strong character of each paragraph (UAX #9 rules P2-P3)
    LeftToRight,
    RightToLeft,
};

class Font {
public:
    // Parses a .ttf/.otf face. `copy` keeps a private copy of the bytes; otherwise `data` must
    // outlive the font (embedded fonts). Fails on data that is not a usable font.
    [[nodiscard]] static Result<std::shared_ptr<Font>> create(std::span<const std::uint8_t> data, std::string name,
                                                              bool copy = true);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool has_glyph(char32_t cp) const noexcept;
    [[nodiscard]] int units_per_em() const noexcept { return upem_; }
    // hhea ascent / descent (descent negative) / line gap in font units.
    [[nodiscard]] int ascent() const noexcept { return ascent_; }
    [[nodiscard]] int descent() const noexcept { return descent_; }
    [[nodiscard]] std::uint32_t glyph_count() const noexcept { return glyphs_; }

    struct Impl;
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

private:
    Font() = default;
    std::string name_;
    std::vector<std::uint8_t> owned_;
    std::unique_ptr<Impl> impl_;
    int upem_{1000}, ascent_{800}, descent_{-200};
    std::uint32_t glyphs_{0};
};

class FontStack {
public:
    FontStack() = default;
    explicit FontStack(std::vector<std::shared_ptr<Font>> faces) : faces_(std::move(faces)) {}
    void add(std::shared_ptr<Font> face) {
        if (face) faces_.push_back(std::move(face));
    }
    [[nodiscard]] std::span<const std::shared_ptr<Font>> faces() const noexcept { return faces_; }
    [[nodiscard]] bool empty() const noexcept { return faces_.empty(); }
    // Index of the first face with a glyph for `cp`, or 0 (the primary face draws .notdef).
    [[nodiscard]] std::uint16_t face_for(char32_t cp) const noexcept;

private:
    std::vector<std::shared_ptr<Font>> faces_;
};

struct LayoutOptions {
    // Font size in pixels with the same meaning as Dear ImGui's and stb_truetype's
    // "pixel height": the primary face's ascent minus descent spans `size` pixels. Fallback faces
    // use the same em size so mixed scripts match.
    float size{16};
    float wrap_width{0}; // > 0 wraps at word boundaries (long words break between characters)
    Direction direction{Direction::Auto};
    float line_height{0}; // pixels between baselines; 0 = `size`
};

struct Glyph {
    std::uint32_t id{0};       // glyph index in faces()[face]
    std::uint16_t face{0};     // index into Layout::faces
    std::uint16_t style{0};    // StyleSpan::style of the glyph's characters (0 when unstyled)
    std::uint32_t cluster{0};  // byte offset into the laid-out text of the cluster's first character
    float x{0}, y{0};          // pen position of the glyph origin relative to the line's left edge and baseline
    float advance{0};
};

struct Line {
    std::vector<Glyph> glyphs;   // visual order, left to right
    float width{0};              // advance width in pixels
    std::uint32_t begin{0}, end{0}; // byte range of the line's characters (end excludes the trailing break)
    bool rtl{false};             // paragraph base direction
};

struct Layout {
    std::vector<Line> lines;
    // Faces referenced by Glyph::face (the stack's faces; for styled layouts every style's faces
    // in order). Shared, so a cached layout keeps its fonts alive.
    std::vector<std::shared_ptr<Font>> faces;
    float size{0};
    float line_height{0};
    float ascent{0};             // primary face ascent in pixels (baseline below the line top)
    float em{0};                 // em size in pixels used for every face
    std::uint32_t missing{0};    // characters no face in the stack could draw (.notdef)
    [[nodiscard]] float width() const noexcept;
    [[nodiscard]] float height() const noexcept { return line_height * static_cast<float>(lines.size()); }
};

// Shapes and lays out UTF-8 `text`. Invalid UTF-8 is replaced with U+FFFD per byte; an empty
// stack produces no glyphs. Never throws.
[[nodiscard]] Layout layout_text(const FontStack& fonts, std::string_view text, const LayoutOptions& options);

// A byte range of the text drawn with font stack `style` (see the styled layout_text).
struct StyleSpan {
    std::uint32_t begin{0}, end{0};
    std::uint16_t style{0};
};

// Styled layout: characters in a span use stacks[span.style] (others use stacks[0]); runs of
// different styles are shaped separately but wrapped and bidi-reordered as one paragraph, so a
// bold word inside Arabic text lands in the right place. Sizes come from stacks[0]'s primary
// face. Null or empty stacks fall back to stacks[0].
[[nodiscard]] Layout layout_text(std::span<const FontStack* const> stacks, std::string_view text, const LayoutOptions& options,
                                 std::span<const StyleSpan> spans);

// Horizontal position (pixels from the line's left edge) of a caret before byte `offset` in the
// line that contains it. Offsets inside a cluster snap to the cluster start; offsets at the end
// of an LTR line return its width (RTL: 0).
[[nodiscard]] float caret_x(const Layout& layout, std::size_t line, std::uint32_t offset) noexcept;

// Line holding byte `offset` (the last line when past the end).
[[nodiscard]] std::size_t line_of(const Layout& layout, std::uint32_t offset) noexcept;

// Rasterised glyph in a GlyphAtlas.
struct AtlasGlyph {
    float u0{0}, v0{0}, u1{0}, v1{0}; // texture coordinates
    float x0{0}, y0{0}, x1{0}, y1{0}; // quad relative to the glyph origin, pixels (y down)
    bool empty{true};                 // nothing to draw (space)
};

struct AtlasStats {
    std::uint32_t glyphs{0};     // resident glyphs
    std::uint32_t rasterised{0}; // glyphs rasterised since the last reset of this counter
    std::uint32_t overflows{0};  // requests that did not fit
    std::uint32_t resets{0};     // times the atlas was cleared to make room
    std::uint32_t grows{0};      // times the atlas doubled in size
    float occupancy{0};          // fraction of rows used
};

// Shelf-packed 8-bit coverage atlas. Glyphs are rasterised at the exact em size (quantised to
// 1/4 pixel), with one pixel of padding so bilinear sampling never bleeds between glyphs.
class GlyphAtlas {
public:
    // Starts at width x height and doubles (width first) up to max_width x max_height when a glyph
    // does not fit; only a full atlas at the maximum size is cleared (see begin_frame).
    explicit GlyphAtlas(std::uint32_t width = 256, std::uint32_t height = 256, std::uint32_t max_width = 2048,
                        std::uint32_t max_height = 2048);
    // Glyph `id` of `font` at `em` pixels; nullptr when the atlas is full at its maximum size.
    // Growing rescales every resident glyph's texture coordinates, so callers that emit vertices
    // should request all of a frame's glyphs before using any coordinates.
    [[nodiscard]] const AtlasGlyph* glyph(const Font& font, std::uint32_t id, float em);
    // Clears the atlas when, at its maximum size, it is more than `max_occupancy` full or a glyph
    // did not fit, so a new frame can refill it. Returns true when it cleared (every previous
    // AtlasGlyph is invalid).
    bool begin_frame(float max_occupancy = 0.9f);
    void clear();

    // Texture coordinates of an opaque 3x3 texel block that glyphs never use: solid fills drawn
    // with it share the atlas texture, so text and rectangles batch into one draw call.
    [[nodiscard]] float white_u() const noexcept { return 1.5f / static_cast<float>(width_); }
    [[nodiscard]] float white_v() const noexcept { return 1.5f / static_cast<float>(height_); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint8_t* mutable_pixels() noexcept { return pixels_.data(); }
    // True when pixels changed since the last call (the caller re-uploads the texture).
    [[nodiscard]] bool take_dirty() noexcept {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }
    [[nodiscard]] AtlasStats& stats() noexcept { return stats_; }

private:
    struct Key {
        const Font* font;
        std::uint32_t id;
        std::uint32_t size_q;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept;
    };
    bool place(int w, int h, int& x, int& y);
    bool grow();

    std::uint32_t width_, height_, max_width_, max_height_;
    std::vector<std::uint8_t> pixels_;
    std::unordered_map<Key, AtlasGlyph, KeyHash> glyphs_;
    int shelf_x_{4}, shelf_y_{1}, shelf_h_{0};
    bool dirty_{true};
    AtlasStats stats_;
};

} // namespace victus::text
