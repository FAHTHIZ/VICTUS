// SPDX-License-Identifier: Apache-2.0
#include "sprite_art.hpp"

#include "victus/render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>

namespace victus::project::detail {
namespace {

struct Rgba {
    float r, g, b, a;
};

// A straight-alpha float canvas painted with 4x4 supersampled shapes and "over" compositing.
class Canvas {
public:
    Canvas(int w, int h, Rgba background) : w_(w), h_(h), px_(static_cast<std::size_t>(w * h), background) {}

    using Shape = std::function<bool(float, float)>;

    void paint(const Shape& inside, Rgba c) {
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                int hits = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                        if (inside(static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 4.0f,
                                   static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 4.0f))
                            ++hits;
                if (hits == 0) continue;
                over(x, y, c, c.a * static_cast<float>(hits) / 16.0f);
            }
    }

    // A vertical gradient filling the whole canvas.
    void gradient(Rgba top, Rgba bottom) {
        for (int y = 0; y < h_; ++y) {
            const float t = static_cast<float>(y) / static_cast<float>(std::max(1, h_ - 1));
            const Rgba c{top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t, top.b + (bottom.b - top.b) * t, 1};
            for (int x = 0; x < w_; ++x) px_[static_cast<std::size_t>(y * w_ + x)] = c;
        }
    }

    [[nodiscard]] std::string png() const {
        render::Image img;
        img.width = static_cast<std::uint32_t>(w_);
        img.height = static_cast<std::uint32_t>(h_);
        img.pixels.resize(px_.size() * 4);
        const auto q = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        for (std::size_t i = 0; i < px_.size(); ++i) {
            img.pixels[i * 4 + 0] = q(px_[i].r);
            img.pixels[i * 4 + 1] = q(px_[i].g);
            img.pixels[i * 4 + 2] = q(px_[i].b);
            img.pixels[i * 4 + 3] = q(px_[i].a);
        }
        auto encoded = render::encode_png(img);
        return encoded ? std::string(encoded->begin(), encoded->end()) : std::string();
    }

private:
    void over(int x, int y, Rgba c, float a) {
        Rgba& d = px_[static_cast<std::size_t>(y * w_ + x)];
        const float out = a + d.a * (1.0f - a);
        if (out <= 0.0f) return;
        const float k = d.a * (1.0f - a);
        d.r = (c.r * a + d.r * k) / out;
        d.g = (c.g * a + d.g * k) / out;
        d.b = (c.b * a + d.b * k) / out;
        d.a = out;
    }

    int w_, h_;
    std::vector<Rgba> px_;
};

Canvas::Shape circle(float cx, float cy, float r) {
    return [=](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
}
Canvas::Shape ellipse(float cx, float cy, float rx, float ry) {
    return [=](float x, float y) {
        const float u = (x - cx) / rx, v = (y - cy) / ry;
        return u * u + v * v <= 1.0f;
    };
}
Canvas::Shape rounded(float x0, float y0, float x1, float y1, float r) {
    return [=](float x, float y) {
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        const float qx = std::max({x0 + r - x, 0.0f, x - (x1 - r)});
        const float qy = std::max({y0 + r - y, 0.0f, y - (y1 - r)});
        return qx * qx + qy * qy <= r * r;
    };
}
Canvas::Shape triangle(float ax, float ay, float bx, float by, float cx, float cy) {
    return [=](float x, float y) {
        const auto side = [&](float px, float py, float qx, float qy) { return (qx - px) * (y - py) - (qy - py) * (x - px); };
        const float d1 = side(ax, ay, bx, by), d2 = side(bx, by, cx, cy), d3 = side(cx, cy, ax, ay);
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        return !(neg && pos);
    };
}
Canvas::Shape both(Canvas::Shape a, Canvas::Shape b) {
    return [=](float x, float y) { return a(x, y) && b(x, y); };
}
Canvas::Shape any(std::vector<Canvas::Shape> parts) {
    return [parts = std::move(parts)](float x, float y) {
        return std::any_of(parts.begin(), parts.end(), [&](const Canvas::Shape& s) { return s(x, y); });
    };
}
// The classic implicit heart (x^2 + y^2 - 1)^3 - x^2 y^3 <= 0, centred at (cx, cy), `scale` px per unit.
Canvas::Shape heart(float cx, float cy, float scale) {
    return [=](float px, float py) {
        const float x = (px - cx) / scale, y = -(py - cy) / scale;
        const float a = x * x + y * y - 1.0f;
        return a * a * a - x * x * y * y * y <= 0.0f;
    };
}

constexpr Rgba kWhite{1, 1, 1, 1};
constexpr Rgba kInk{0.12f, 0.1f, 0.14f, 1};

void eyes(Canvas& c, float lx, float rx, float y, float r, float look) {
    for (const float x : {lx, rx}) {
        c.paint(ellipse(x, y, r, r * 1.2f), kWhite);
        c.paint(circle(x + look, y + r * 0.15f, r * 0.5f), kInk);
        c.paint(circle(x + look - r * 0.2f, y - r * 0.15f, r * 0.17f), kWhite);
    }
}

std::string player() {
    Canvas c(128, 128, {0.98f, 0.52f, 0.18f, 0});
    c.paint(rounded(6, 8, 122, 124, 28), {0.5f, 0.2f, 0.06f, 1});
    c.paint(rounded(12, 14, 116, 118, 23), {0.98f, 0.52f, 0.18f, 1});
    c.paint(rounded(12, 84, 116, 118, 23), {0.9f, 0.42f, 0.12f, 1});
    c.paint(rounded(22, 22, 96, 44, 11), {1.0f, 0.72f, 0.42f, 0.7f});
    eyes(c, 70, 98, 60, 12, 4);
    c.paint(rounded(78, 86, 100, 93, 3.5f), {0.42f, 0.14f, 0.06f, 1});
    c.paint(ellipse(56, 82, 8, 5), {1.0f, 0.42f, 0.4f, 0.55f});
    return c.png();
}

std::string slime() {
    Canvas c(128, 112, {0.42f, 0.85f, 0.35f, 0});
    c.paint(both(ellipse(64, 112, 62, 104), [](float, float y) { return y <= 110; }), {0.14f, 0.36f, 0.12f, 1});
    c.paint(both(ellipse(64, 112, 56, 98), [](float, float y) { return y <= 104; }), {0.42f, 0.85f, 0.35f, 1});
    c.paint(both(ellipse(64, 112, 56, 40), [](float, float y) { return y <= 104; }), {0.34f, 0.74f, 0.28f, 1});
    c.paint(ellipse(42, 40, 12, 7), {1, 1, 1, 0.6f});
    eyes(c, 48, 80, 62, 11, -3);
    c.paint(triangle(34, 44, 58, 50, 58, 45), kInk); // angry brows
    c.paint(triangle(94, 44, 70, 50, 70, 45), kInk);
    return c.png();
}

std::string spikes() {
    Canvas c(192, 80, {0.82f, 0.84f, 0.88f, 0});
    for (int i = 0; i < 3; ++i) {
        const float x = 32.0f + 64.0f * static_cast<float>(i);
        c.paint(triangle(x - 31, 79, x, 1, x + 31, 79), {0.22f, 0.22f, 0.28f, 1});
        c.paint(triangle(x - 25, 76, x, 10, x + 25, 76), {0.86f, 0.88f, 0.92f, 1});
        c.paint(triangle(x, 76, x, 10, x + 25, 76), {0.6f, 0.62f, 0.7f, 1});
    }
    return c.png();
}

std::string coin() {
    Canvas c(64, 64, {1.0f, 0.8f, 0.15f, 0});
    c.paint(circle(32, 32, 31), {0.72f, 0.42f, 0.04f, 1});
    c.paint(circle(32, 32, 27), {1.0f, 0.8f, 0.16f, 1});
    c.paint([](float x, float y) {
        const float d = std::hypot(x - 32, y - 32);
        return d <= 19 && d >= 15.5f;
    }, {0.88f, 0.6f, 0.08f, 1});
    c.paint(rounded(29, 20, 35, 44, 3), {0.88f, 0.6f, 0.08f, 1});
    c.paint(ellipse(22, 20, 7, 4), {1, 1, 0.9f, 0.75f});
    return c.png();
}

std::string heart_art() {
    Canvas c(64, 64, {0.95f, 0.25f, 0.4f, 0});
    c.paint(heart(32, 30, 28), {0.45f, 0.06f, 0.14f, 1});
    c.paint(heart(32, 30, 24), {0.95f, 0.25f, 0.4f, 1});
    c.paint(ellipse(22, 20, 6, 4), {1, 0.85f, 0.9f, 0.8f});
    return c.png();
}

// White and grey so the sprite tint colours it: gold for the goal, blue for checkpoints.
std::string flag() {
    Canvas c(96, 240, {0.9f, 0.9f, 0.9f, 0});
    c.paint(rounded(7, 14, 17, 240, 3), {0.35f, 0.33f, 0.36f, 1});
    c.paint(rounded(9, 14, 15, 240, 2), {0.82f, 0.82f, 0.85f, 1});
    c.paint(circle(12, 12, 10), {0.35f, 0.33f, 0.36f, 1});
    c.paint(circle(12, 12, 7.5f), kWhite);
    c.paint(triangle(16, 26, 94, 58, 16, 92), {0.4f, 0.4f, 0.44f, 1});
    c.paint(triangle(18, 32, 86, 58, 18, 85), kWhite);
    c.paint(triangle(18, 62, 86, 58, 18, 85), {0.82f, 0.82f, 0.86f, 1});
    return c.png();
}

// A red spring pad on a grey coil (jump pads).
std::string spring() {
    Canvas c(128, 64, {0.9f, 0.2f, 0.2f, 0});
    c.paint(rounded(20, 52, 108, 64, 4), {0.25f, 0.25f, 0.3f, 1}); // base
    for (int i = 0; i < 3; ++i) {
        const float y = 22.0f + 10.0f * static_cast<float>(i);
        c.paint(rounded(32, y, 96, y + 6, 3), {0.4f, 0.4f, 0.46f, 1});
        c.paint(rounded(34, y + 1, 94, y + 3, 1), {0.78f, 0.8f, 0.86f, 1});
    }
    c.paint(rounded(4, 2, 124, 22, 8), {0.55f, 0.08f, 0.1f, 1}); // pad
    c.paint(rounded(8, 4, 120, 18, 6), {0.93f, 0.25f, 0.24f, 1});
    c.paint(ellipse(36, 9, 16, 3), {1, 0.75f, 0.72f, 0.8f});
    return c.png();
}

// A wooden notice board on a post (signs).
std::string sign_art() {
    Canvas c(96, 96, {0.62f, 0.42f, 0.24f, 0});
    c.paint(rounded(42, 40, 54, 96, 2), {0.36f, 0.22f, 0.12f, 1});
    c.paint(rounded(4, 6, 92, 58, 8), {0.36f, 0.22f, 0.12f, 1});
    c.paint(rounded(8, 10, 88, 54, 6), {0.74f, 0.52f, 0.3f, 1});
    for (int i = 0; i < 3; ++i) {
        const float y = 20.0f + 11.0f * static_cast<float>(i);
        c.paint(rounded(18, y, i == 2 ? 58.0f : 78.0f, y + 4, 2), {0.45f, 0.29f, 0.16f, 1});
    }
    return c.png();
}

std::string cloud() {
    Canvas c(192, 96, {1, 1, 1, 0});
    const auto shape = any({circle(60, 58, 32), circle(100, 42, 38), circle(140, 60, 30), rounded(28, 56, 168, 92, 26)});
    c.paint(shape, kWhite);
    c.paint(both(shape, [](float, float y) { return y > 74; }), {0.86f, 0.91f, 1.0f, 1});
    return c.png();
}

std::string round_art() {
    Canvas c(128, 128, {1, 1, 1, 0});
    c.paint(circle(64, 64, 62), kWhite);
    return c.png();
}

std::string sky() {
    Canvas c(8, 128, {0, 0, 0, 1});
    c.gradient({0.3f, 0.52f, 0.9f, 1}, {0.74f, 0.88f, 1.0f, 1});
    return c.png();
}

struct Entry {
    const char* name;
    const char* uuid;
    std::string (*draw)();
};
constexpr Entry kArt[] = {
    {"player", "7a1c0de0-2b4e-4f6a-9c1d-000000000001", player},
    {"slime", "7a1c0de0-2b4e-4f6a-9c1d-000000000002", slime},
    {"spikes", "7a1c0de0-2b4e-4f6a-9c1d-000000000003", spikes},
    {"coin", "7a1c0de0-2b4e-4f6a-9c1d-000000000004", coin},
    {"heart", "7a1c0de0-2b4e-4f6a-9c1d-000000000005", heart_art},
    {"flag", "7a1c0de0-2b4e-4f6a-9c1d-000000000006", flag},
    {"cloud", "7a1c0de0-2b4e-4f6a-9c1d-000000000007", cloud},
    {"round", "7a1c0de0-2b4e-4f6a-9c1d-000000000008", round_art},
    {"sky", "7a1c0de0-2b4e-4f6a-9c1d-000000000009", sky},
    {"spring", "7a1c0de0-2b4e-4f6a-9c1d-00000000000a", spring},
    {"sign", "7a1c0de0-2b4e-4f6a-9c1d-00000000000b", sign_art},
};

} // namespace

std::vector<SpriteArt> nocode_sprite_art() {
    std::vector<SpriteArt> out;
    out.reserve(std::size(kArt));
    for (const Entry& e : kArt) out.push_back({e.name, e.uuid, e.draw()});
    return out;
}

const char* nocode_sprite_uuid(const char* name) {
    for (const Entry& e : kArt)
        if (std::strcmp(e.name, name) == 0) return e.uuid;
    return nullptr;
}

} // namespace victus::project::detail
