// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/ies.hpp"

#include "victus/core/charconv.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace victus::assets {
namespace {

// Splits on whitespace and commas (LM-63 allows both between numbers).
class Numbers {
public:
    explicit Numbers(std::string_view s) : s_(s) {}
    bool next(double& v) {
        while (pos_ < s_.size() && (std::isspace(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == ',')) ++pos_;
        if (pos_ >= s_.size()) return false;
        std::size_t end = pos_;
        while (end < s_.size() && !std::isspace(static_cast<unsigned char>(s_[end])) && s_[end] != ',') ++end;
        const auto r = from_chars_double(s_.data() + pos_, s_.data() + end, v);
        if (r.ec != std::errc{} || r.ptr != s_.data() + end || !std::isfinite(v)) return false;
        pos_ = end;
        return true;
    }
    bool next(float& v) {
        double d = 0.0;
        if (!next(d)) return false;
        v = static_cast<float>(d);
        return true;
    }

private:
    std::string_view s_;
    std::size_t pos_{0};
};

Error bad(std::string_view what) { return make_error(ErrorCode::ParseError, std::format("IES: {}", what)); }

// Index i and weight t with xs[i] <= x <= xs[i + 1] (xs ascending, x inside the range).
void bracket(const std::vector<float>& xs, float x, std::size_t& i, float& t) {
    if (xs.size() == 1 || x <= xs.front()) {
        i = 0, t = 0.0f;
        return;
    }
    if (x >= xs.back()) {
        i = xs.size() - 1, t = 0.0f;
        return;
    }
    i = static_cast<std::size_t>(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin()) - 1;
    const float span = xs[i + 1] - xs[i];
    t = span > 0.0f ? (x - xs[i]) / span : 0.0f;
}

} // namespace

float IesProfile::sample(float vertical_deg, float horizontal_deg) const noexcept {
    if (vertical.empty() || horizontal.empty() || candela.size() != vertical.size() * horizontal.size()) return 0.0f;
    if (vertical_deg < vertical.front() - 1e-3f || vertical_deg > vertical.back() + 1e-3f) return 0.0f;
    // Fold the horizontal angle by the file's symmetry (LM-63 section 4.x).
    float h = std::fmod(horizontal_deg, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const float first = horizontal.front(), last = horizontal.back();
    if (horizontal.size() == 1) {
        h = first; // rotationally symmetric
    } else if (first == 0.0f && last == 90.0f) {
        if (h > 180.0f) h = 360.0f - h;
        if (h > 90.0f) h = 180.0f - h;
    } else if (first == 0.0f && last == 180.0f) {
        if (h > 180.0f) h = 360.0f - h;
    } else if (first == 90.0f && last == 270.0f) {
        if (h < 90.0f) h = 180.0f - h;
        else if (h > 270.0f) h = 540.0f - h;
    }
    const std::size_t nv = vertical.size();
    const auto at = [&](std::size_t hi, float v) {
        std::size_t vi = 0;
        float vt = 0.0f;
        bracket(vertical, v, vi, vt);
        const float a = candela[hi * nv + vi];
        return vi + 1 < nv ? a + (candela[hi * nv + vi + 1] - a) * vt : a;
    };
    if (horizontal.size() == 1) return at(0, vertical_deg);
    if (h > last) { // full 0..<360 tables without a closing 360 column: wrap to the first column
        const float span = 360.0f + first - last;
        const float t = span > 0.0f ? (h - last) / span : 0.0f;
        return at(horizontal.size() - 1, vertical_deg) * (1.0f - t) + at(0, vertical_deg) * t;
    }
    if (h < first) return at(0, vertical_deg);
    std::size_t hi = 0;
    float ht = 0.0f;
    bracket(horizontal, h, hi, ht);
    const float a = at(hi, vertical_deg);
    return hi + 1 < horizontal.size() ? a + (at(hi + 1, vertical_deg) - a) * ht : a;
}

Result<IesProfile> parse_ies(std::string_view text) {
    // Keyword lines up to and including TILT=.
    std::size_t pos = 0;
    std::string_view tilt;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.starts_with("TILT=") || line.starts_with("TILT =")) {
            tilt = line.substr(line.find('=') + 1);
            while (!tilt.empty() && tilt.front() == ' ') tilt.remove_prefix(1);
            break;
        }
    }
    if (tilt.empty()) return bad("no TILT= line (not an LM-63 photometric file)");
    if (pos > text.size()) pos = text.size();
    Numbers n(text.substr(pos));
    if (tilt == "INCLUDE") {
        // Lamp-to-luminaire geometry, the number of tilt angles, then angles and factors.
        double geometry = 0.0, pairs = 0.0, skip = 0.0;
        if (!n.next(geometry) || !n.next(pairs) || pairs < 0 || pairs > 10000) return bad("bad TILT=INCLUDE table");
        for (int i = 0; i < 2 * static_cast<int>(pairs); ++i)
            if (!n.next(skip)) return bad("truncated TILT=INCLUDE table");
    } else if (tilt != "NONE") {
        return bad("TILT=<file> is not supported (only NONE or INCLUDE)");
    }
    double lamps = 0, lumens = 0, multiplier = 0, nv = 0, nh = 0, type = 0, units = 0, w = 0, l = 0, h = 0;
    double ballast = 0, future = 0, watts = 0;
    if (!n.next(lamps) || !n.next(lumens) || !n.next(multiplier) || !n.next(nv) || !n.next(nh) || !n.next(type) ||
        !n.next(units) || !n.next(w) || !n.next(l) || !n.next(h) || !n.next(ballast) || !n.next(future) || !n.next(watts))
        return bad("truncated header numbers");
    if (type != 1.0) return bad(std::format("photometric type {} is not supported (only Type C)", type));
    if (nv < 1 || nh < 1 || nv > 1024 || nh > 1024) return bad("angle counts out of range");
    IesProfile p;
    p.lamps = static_cast<int>(lamps);
    p.lumens_per_lamp = static_cast<float>(lumens);
    p.vertical.resize(static_cast<std::size_t>(nv));
    p.horizontal.resize(static_cast<std::size_t>(nh));
    for (float& a : p.vertical)
        if (!n.next(a)) return bad("truncated vertical angles");
    for (float& a : p.horizontal)
        if (!n.next(a)) return bad("truncated horizontal angles");
    if (!std::is_sorted(p.vertical.begin(), p.vertical.end()) || !std::is_sorted(p.horizontal.begin(), p.horizontal.end()))
        return bad("angles are not ascending");
    if (p.vertical.front() < 0.0f || p.vertical.back() > 180.0f) return bad("vertical angles outside 0..180");
    p.candela.resize(p.vertical.size() * p.horizontal.size());
    const float scale = static_cast<float>(multiplier * (ballast > 0.0 ? ballast : 1.0));
    for (float& c : p.candela) {
        if (!n.next(c)) return bad("truncated candela values");
        c = std::max(c * scale, 0.0f);
        p.peak_candela = std::max(p.peak_candela, c);
    }
    return p;
}

render::Image ies_cookie_image(const IesProfile& profile, std::uint32_t size) {
    render::Image img;
    img.width = img.height = std::max<std::uint32_t>(size, 4);
    img.pixels.assign(std::size_t{img.width} * img.height * 4, 255);
    const float inv_peak = profile.peak_candela > 0.0f ? 1.0f / profile.peak_candela : 0.0f;
    for (std::uint32_t y = 0; y < img.height; ++y) {
        // Row centre: angle from straight up is v * 180 degrees; IES vertical 0 is straight down.
        const float vertical = 180.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(img.height) * 180.0f;
        for (std::uint32_t x = 0; x < img.width; ++x) {
            // Column centre: atan2(right, forward) = (u - 1/2) * 360 degrees.
            const float horizontal = ((static_cast<float>(x) + 0.5f) / static_cast<float>(img.width) - 0.5f) * 360.0f;
            const float c = std::clamp(profile.sample(vertical, horizontal) * inv_peak, 0.0f, 1.0f);
            const auto g = static_cast<std::uint8_t>(std::lround(math::linear_to_srgb(c) * 255.0f));
            std::uint8_t* px = &img.pixels[(std::size_t{y} * img.width + x) * 4];
            px[0] = px[1] = px[2] = g;
        }
    }
    return img;
}

} // namespace victus::assets
