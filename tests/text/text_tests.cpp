// SPDX-License-Identifier: Apache-2.0
// Text shaping and bidirectional layout tests (ADR-0224): Bengali conjuncts, Arabic joining,
// mixed-direction reordering, font fallback, wrapping, carets, atlas and hostile input.
#include "victus/text/text.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace victus;
using namespace victus::text;

namespace {

std::vector<std::uint8_t> read_file(const std::string& rel) {
    std::ifstream in(std::string(VICTUS_SOURCE_DIR) + "/" + rel, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::shared_ptr<Font> load(const std::string& rel) {
    auto bytes = read_file(rel);
    REQUIRE_FALSE(bytes.empty());
    auto font = Font::create(bytes, rel);
    REQUIRE(font.ok());
    return *font;
}

const FontStack& stack() {
    static const FontStack fonts = [] {
        FontStack s;
        s.add(load("third_party/fonts/inter/Inter-Medium.ttf"));
        s.add(load("third_party/fonts/noto/NotoSansBengali-Medium.ttf"));
        s.add(load("third_party/fonts/noto/NotoSansArabic-Medium.ttf"));
        return s;
    }();
    return fonts;
}

std::size_t glyph_count(const Layout& l) {
    std::size_t n = 0;
    for (const auto& line : l.lines) n += line.glyphs.size();
    return n;
}

bool has_notdef(const Layout& l) {
    for (const auto& line : l.lines)
        for (const auto& g : line.glyphs)
            if (g.id == 0) return true;
    return false;
}

} // namespace

TEST_CASE("text: Latin layout measures and uses the primary face") {
    const Layout l = layout_text(stack(), "Hello", {.size = 32});
    REQUIRE(l.lines.size() == 1);
    CHECK(l.lines[0].glyphs.size() == 5);
    CHECK(l.missing == 0);
    CHECK(l.lines[0].width > 40.0f);
    CHECK(l.lines[0].width < 160.0f);
    for (const auto& g : l.lines[0].glyphs) CHECK(g.face == 0);
    // Glyphs advance left to right.
    for (std::size_t i = 1; i < 5; ++i) CHECK(l.lines[0].glyphs[i].x > l.lines[0].glyphs[i - 1].x);
}

TEST_CASE("text: Bengali conjunct is shaped into fewer glyphs than code points") {
    // KA + VIRAMA + SSA forms the conjunct KSSA; a font without shaping would draw 3 glyphs.
    const std::string ksa = "\xE0\xA6\x95\xE0\xA7\x8D\xE0\xA6\xB7";
    const Layout l = layout_text(stack(), ksa, {.size = 32});
    REQUIRE(l.lines.size() == 1);
    CHECK(l.missing == 0);
    CHECK_FALSE(has_notdef(l));
    CHECK(l.lines[0].glyphs.size() < 3);
    for (const auto& g : l.lines[0].glyphs) CHECK(g.face == 1);
    // "বাংলা" (Bangla) shapes fully with the Bengali face.
    const Layout bangla = layout_text(stack(), "\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\x82\xE0\xA6\xB2\xE0\xA6\xBE", {.size = 32});
    CHECK(bangla.missing == 0);
    CHECK_FALSE(has_notdef(bangla));
}

TEST_CASE("text: Bengali pre-base vowel sign is reordered before its consonant") {
    // KA + VOWEL SIGN E (U+09C7): the vowel sign is drawn to the left of KA, so the leftmost glyph
    // belongs to the same cluster as KA (offset 0) but is not KA's base glyph alone.
    const std::string ke = "\xE0\xA6\x95\xE0\xA7\x87";
    const Layout l = layout_text(stack(), ke, {.size = 32});
    REQUIRE(l.lines.size() == 1);
    REQUIRE(l.lines[0].glyphs.size() == 2);
    CHECK(l.missing == 0);
    // Unshaped, glyph 0 would be KA; shaped, glyph 0 is the E sign. Compare with KA alone.
    const Layout ka = layout_text(stack(), "\xE0\xA6\x95", {.size = 32});
    REQUIRE(ka.lines[0].glyphs.size() == 1);
    CHECK(l.lines[0].glyphs[0].id != ka.lines[0].glyphs[0].id);
    CHECK(l.lines[0].glyphs[1].id == ka.lines[0].glyphs[0].id);
}

TEST_CASE("text: Arabic is laid out right to left with contextual forms") {
    const std::string salam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"; // سلام
    const Layout l = layout_text(stack(), salam, {.size = 32});
    REQUIRE(l.lines.size() == 1);
    const Line& line = l.lines[0];
    CHECK(line.rtl);
    CHECK(l.missing == 0);
    CHECK_FALSE(has_notdef(l));
    REQUIRE(line.glyphs.size() >= 3);
    for (const auto& g : line.glyphs) CHECK(g.face == 2);
    // The first logical character (SEEN, cluster 0) is drawn rightmost.
    CHECK(line.glyphs.back().cluster == 0);
    // Joining: an initial SEEN differs from an isolated one.
    const Layout seen = layout_text(stack(), "\xD8\xB3", {.size = 32});
    REQUIRE(seen.lines[0].glyphs.size() == 1);
    CHECK(line.glyphs.back().id != seen.lines[0].glyphs[0].id);
}

TEST_CASE("text: mixed-direction text follows the bidirectional algorithm") {
    const std::string mixed = "Hello \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 123";
    const Layout l = layout_text(stack(), mixed, {.size = 24});
    REQUIRE(l.lines.size() == 1);
    const Line& line = l.lines[0];
    CHECK_FALSE(line.rtl); // first strong character is Latin
    CHECK(l.missing == 0);
    // UAX #9 rule W2 turns digits after Arabic letters into Arabic numbers, so the RTL run is
    // "سلام 123": visually "Hello ", then the digits (still left to right), then the Arabic with its
    // first letter rightmost.
    CHECK(line.glyphs.front().cluster == 0);
    const auto pos = [&](std::uint32_t cluster) {
        for (const auto& g : line.glyphs)
            if (g.cluster == cluster) return g.x;
        return -1.0f;
    };
    const float o_x = pos(4), seen_x = pos(6), meem_x = pos(12), one_x = pos(15), three_x = pos(17);
    REQUIRE(seen_x >= 0);
    REQUIRE(meem_x >= 0);
    REQUIRE(one_x >= 0);
    REQUIRE(three_x >= 0);
    CHECK(meem_x < seen_x);  // RTL run: last letter left of the first
    CHECK(one_x < three_x);  // numbers keep left-to-right order
    CHECK(three_x < meem_x); // ...and sit left of the Arabic word
    CHECK(o_x < one_x);      // after "Hello"
    // Latin digits after Latin text stay where they are.
    const Layout latin = layout_text(stack(), "abc 123", {.size = 24});
    CHECK(latin.lines[0].glyphs.back().cluster == 6);
    // Forced RTL base direction puts the Latin word on the right.
    const Layout r = layout_text(stack(), mixed, {.size = 24, .direction = Direction::RightToLeft});
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].rtl);
    CHECK(r.lines[0].glyphs.back().cluster <= 4);
}

TEST_CASE("text: wrapping keeps lines within the width and breaks at spaces") {
    const std::string text = "The quick brown fox jumps over the lazy dog";
    const Layout l = layout_text(stack(), text, {.size = 20, .wrap_width = 120});
    REQUIRE(l.lines.size() > 2);
    for (const auto& line : l.lines) CHECK(line.width <= 120.5f);
    // Every line after the first starts at a word.
    for (std::size_t i = 1; i < l.lines.size(); ++i) CHECK(text[l.lines[i].begin - 1] == ' ');
    CHECK(l.height() == doctest::Approx(l.line_height * static_cast<float>(l.lines.size())));
    // Explicit newlines start paragraphs.
    const Layout p = layout_text(stack(), "a\nb\n\nc", {.size = 20});
    CHECK(p.lines.size() == 4);
    // A single long word still breaks.
    const Layout w = layout_text(stack(), "Supercalifragilisticexpialidocious", {.size = 20, .wrap_width = 60});
    CHECK(w.lines.size() > 1);
    for (const auto& line : w.lines) CHECK(line.width <= 60.5f);
}

TEST_CASE("text: caret positions are monotonic in LTR and mirrored in RTL") {
    const std::string s = "Caret";
    const Layout l = layout_text(stack(), s, {.size = 20});
    float prev = -1;
    for (std::uint32_t i = 0; i <= s.size(); ++i) {
        const float x = caret_x(l, 0, i);
        CHECK(x > prev);
        prev = x;
    }
    CHECK(caret_x(l, 0, static_cast<std::uint32_t>(s.size())) == doctest::Approx(l.lines[0].width));
    const std::string salam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";
    const Layout r = layout_text(stack(), salam, {.size = 20});
    CHECK(caret_x(r, 0, 0) == doctest::Approx(r.lines[0].width));
    CHECK(caret_x(r, 0, static_cast<std::uint32_t>(salam.size())) == doctest::Approx(0.0f));
    CHECK(caret_x(r, 0, 2) < caret_x(r, 0, 0));
    const Layout two = layout_text(stack(), "ab\ncd", {.size = 20});
    CHECK(line_of(two, 0) == 0);
    CHECK(line_of(two, 4) == 1);
    CHECK(line_of(two, 99) == 1);
}

TEST_CASE("text: characters no face covers are counted as missing") {
    const Layout l = layout_text(stack(), "A\xE4\xB8\xAD", {.size = 20}); // A + CJK "中"
    CHECK(l.missing == 1);
}

TEST_CASE("text: hostile input is rejected or handled safely") {
    const std::vector<std::uint8_t> junk(64, 0xAB);
    CHECK_FALSE(Font::create(junk, "junk").ok());
    CHECK_FALSE(Font::create({}, "empty").ok());
    auto truncated = read_file("third_party/fonts/inter/Inter-Medium.ttf");
    truncated.resize(200);
    CHECK_FALSE(Font::create(truncated, "truncated").ok());
    // Invalid UTF-8 becomes replacement characters, never a crash.
    const Layout bad = layout_text(stack(), std::string("a\xFF\xC3", 3), {.size = 20});
    REQUIRE(bad.lines.size() == 1);
    CHECK(bad.lines[0].glyphs.size() >= 2);
    // Empty stack / empty text / absurd sizes.
    CHECK(glyph_count(layout_text(FontStack{}, "abc", {.size = 20})) == 0);
    CHECK(layout_text(stack(), "", {.size = 20}).lines.size() == 1);
    CHECK(glyph_count(layout_text(stack(), "abc", {.size = 0})) == 0);
    CHECK(glyph_count(layout_text(stack(), "abc", {.size = -5})) == 0);
}

TEST_CASE("text: glyph atlas rasterises shaped glyphs and recycles when full") {
    GlyphAtlas atlas(256, 256, 256, 256);
    const Layout l = layout_text(stack(), "Hi \xE0\xA6\x95\xE0\xA7\x8D\xE0\xA6\xB7 \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", {.size = 32});
    std::size_t drawn = 0;
    for (const auto& g : l.lines[0].glyphs) {
        const AtlasGlyph* a = atlas.glyph(*stack().faces()[g.face], g.id, l.em);
        REQUIRE(a != nullptr);
        if (!a->empty) {
            ++drawn;
            CHECK(a->u1 > a->u0);
            CHECK(a->v1 > a->v0);
            CHECK(a->x1 > a->x0);
        }
    }
    CHECK(drawn >= 5);
    CHECK(atlas.take_dirty());
    CHECK_FALSE(atlas.take_dirty());
    std::size_t covered = 0;
    for (auto p : atlas.pixels()) covered += p > 128 ? 1 : 0;
    CHECK(covered > 200);
    // Cached glyphs are not rasterised again.
    const auto before = atlas.stats().rasterised;
    (void)atlas.glyph(*stack().faces()[0], l.lines[0].glyphs[0].id, l.em);
    CHECK(atlas.stats().rasterised == before);
    // Fill until overflow, then begin_frame clears.
    for (std::uint32_t id = 1; id < 400; ++id) (void)atlas.glyph(*stack().faces()[0], id, 48);
    CHECK(atlas.stats().overflows > 0);
    CHECK(atlas.begin_frame());
    CHECK(atlas.stats().glyphs == 0);
    CHECK(atlas.stats().grows == 0);
}

TEST_CASE("text: glyph atlas grows before recycling and keeps glyphs and the white texel valid") {
    GlyphAtlas atlas(64, 64, 512, 256);
    const auto white = [&] {
        const auto x = static_cast<std::size_t>(atlas.white_u() * static_cast<float>(atlas.width()));
        const auto y = static_cast<std::size_t>(atlas.white_v() * static_cast<float>(atlas.height()));
        return atlas.pixels()[y * atlas.width() + x];
    };
    CHECK(white() == 255);
    const Font& face = *stack().faces()[0];
    const std::uint32_t first_id = 36; // a Latin capital in Inter's subset
    const AtlasGlyph first = *atlas.glyph(face, first_id, 40);
    REQUIRE_FALSE(first.empty);
    // Remember the first glyph's pixels in atlas coordinates.
    const auto sample = [&](const AtlasGlyph& g) {
        std::vector<std::uint8_t> out;
        const auto x0 = static_cast<std::size_t>(std::lround(g.u0 * static_cast<float>(atlas.width())));
        const auto y0 = static_cast<std::size_t>(std::lround(g.v0 * static_cast<float>(atlas.height())));
        const auto w = static_cast<std::size_t>(g.x1 - g.x0), h = static_cast<std::size_t>(g.y1 - g.y0);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) out.push_back(atlas.pixels()[(y0 + y) * atlas.width() + x0 + x]);
        return out;
    };
    const auto before = sample(first);
    for (std::uint32_t id = 1; id < 120; ++id) (void)atlas.glyph(face, id, 40);
    CHECK(atlas.stats().grows >= 2);
    CHECK(atlas.width() > 64);
    CHECK(atlas.height() > 64);
    CHECK(atlas.width() <= 512);
    CHECK(atlas.height() <= 256);
    CHECK(white() == 255);
    const AtlasGlyph* again = atlas.glyph(face, first_id, 40);
    REQUIRE(again != nullptr);
    CHECK(sample(*again) == before);
    // Only a full atlas at its maximum size is cleared.
    for (int em = 41; em < 80; ++em)
        for (std::uint32_t id = 1; id < 200; ++id) (void)atlas.glyph(face, id, static_cast<float>(em));
    CHECK(atlas.width() == 512);
    CHECK(atlas.height() == 256);
    CHECK(atlas.stats().overflows > 0);
    CHECK(atlas.begin_frame());
    CHECK(atlas.width() == 512);
    CHECK(white() == 255);
    CHECK_FALSE(atlas.begin_frame());
}

TEST_CASE("text: styled spans shape per style but reorder as one paragraph") {
    FontStack bold;
    bold.add(load("third_party/fonts/inter/Inter-Bold.ttf"));
    bold.add(load("third_party/fonts/noto/NotoSansArabic-Bold.ttf"));
    const FontStack* stacks[] = {&stack(), &bold};
    // "سلام " regular, "abc" bold, " مرحبا" regular in an RTL paragraph.
    const std::string t = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 abc \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
    const StyleSpan spans[] = {{9, 12, 1}};
    const Layout l = layout_text(stacks, t, {.size = 24}, spans);
    REQUIRE(l.lines.size() == 1);
    CHECK(l.lines[0].rtl);
    CHECK(l.faces.size() == 5);
    CHECK(l.missing == 0);
    float bold_min = 1e9f, bold_max = -1e9f, first_word = -1, last_word = 1e9f;
    for (const auto& g : l.lines[0].glyphs) {
        if (g.cluster >= 9 && g.cluster < 12) {
            CHECK(g.style == 1);
            CHECK(l.faces[g.face]->name().find("Inter-Bold") != std::string::npos);
            bold_min = std::min(bold_min, g.x);
            bold_max = std::max(bold_max, g.x);
        } else {
            CHECK(g.style == 0);
            if (g.cluster == 0) first_word = g.x;
            if (g.cluster == 13) last_word = g.x;
        }
    }
    // RTL paragraph: the first Arabic word is rightmost, the last leftmost, the bold Latin between.
    CHECK(first_word > bold_max);
    CHECK(last_word < bold_min);
    // Out-of-range styles and spans are ignored safely.
    const StyleSpan bad[] = {{0, 9999, 7}, {5, 2, 1}, {100, 200, 1}};
    const Layout b = layout_text(stacks, t, {.size = 24}, bad);
    CHECK(b.lines.size() == 1);
}

TEST_CASE("text: deterministic fuzz of text, options and corrupted fonts") {
    std::uint32_t seed = 12345;
    const auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const char* pieces[] = {"a", " ", "\n", "\xD8\xB3", "\xE0\xA6\x95", "\xE0\xA7\x8D", "1", "\xE2\x80\x8F", "\xE2\x80\xAE",
                            "-", "\xFF", "\xC3", "\xE2\x81\xA6", "\xCC\x81", "\r\n", "\xE2\x80\x8B"};
    GlyphAtlas atlas(128, 128, 512, 512);
    for (int it = 0; it < 2000; ++it) {
        std::string t;
        const auto n = rnd() % 32;
        for (std::uint32_t i = 0; i < n; ++i) t += pieces[rnd() % std::size(pieces)];
        LayoutOptions o;
        o.size = static_cast<float>(rnd() % 60);
        o.wrap_width = static_cast<float>(rnd() % 100);
        o.direction = static_cast<Direction>(rnd() % 3);
        const Layout l = layout_text(stack(), t, o);
        for (std::size_t li = 0; li < l.lines.size(); ++li) {
            CHECK(l.lines[li].begin <= l.lines[li].end);
            CHECK(l.lines[li].end <= t.size());
            for (std::uint32_t off = 0; off <= t.size(); ++off) {
                const float x = caret_x(l, li, off);
                CHECK(x >= -0.01f);
                CHECK(x <= l.lines[li].width + 0.01f);
            }
            for (const auto& g : l.lines[li].glyphs) (void)atlas.glyph(*l.faces[g.face], g.id, l.em);
        }
        atlas.begin_frame();
    }
    const auto base = read_file("third_party/fonts/noto/NotoSansArabic-Medium.ttf");
    for (int it = 0; it < 100; ++it) {
        auto bytes = base;
        for (int k = 0; k < 20; ++k) bytes[rnd() % bytes.size()] = static_cast<std::uint8_t>(rnd());
        bytes.resize(rnd() % bytes.size() + 1);
        auto f = Font::create(bytes, "mutated");
        if (!f) continue;
        FontStack fs;
        fs.add(*f);
        const Layout l = layout_text(fs, "\xD8\xB3\xD9\x84 abc", {.size = 20});
        for (const auto& line : l.lines)
            for (const auto& g : line.glyphs) (void)atlas.glyph(**f, g.id, 20);
    }
}
