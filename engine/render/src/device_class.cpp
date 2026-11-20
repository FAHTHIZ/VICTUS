// SPDX-License-Identifier: Apache-2.0
#include "victus/render/device_class.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string_view>

namespace victus::render {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The first run of digits at or after `from` (skipping spaces, "(tm)" and similar), if any.
std::optional<int> number_after(std::string_view s, std::size_t from) {
    std::size_t i = from;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (i - from > 8) return std::nullopt; // too far: the digits belong to something else
        ++i;
    }
    if (i >= s.size()) return std::nullopt;
    int n = 0;
    std::size_t digits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && digits < 5) {
        n = n * 10 + (s[i] - '0');
        ++i;
        ++digits;
    }
    return n;
}

QualityTier lower_of(QualityTier a, QualityTier b) { return static_cast<int>(a) < static_cast<int>(b) ? a : b; }

// Mali: Midgard (T-series) and the small Bifrost/Valhall parts are entry level; G7x and G6xx are
// mid-range; G7xx/G9xx and Immortalis are flagship.
QualityTier mali_tier(std::string_view g, std::size_t at) {
    if (g.find("immortalis") != std::string_view::npos) return QualityTier::High;
    const std::size_t after = at + 5; // "mali-"
    if (after < g.size() && g[after] == 't') return QualityTier::Low;
    const auto n = number_after(g, after);
    if (!n) return QualityTier::Low;
    if (*n < 100) return (*n >= 71 && *n <= 78) ? QualityTier::Medium : QualityTier::Low; // G71-G78 vs G31-G68
    const int series = *n / 100;
    if (series <= 3) return QualityTier::Low;  // G310
    if (series <= 6) return QualityTier::Medium; // G610, G615
    return QualityTier::High;                  // G710, G715, G720, G925
}

// Adreno: 5xx and the small 6xx are entry level; 620-649 and 710/720 mid-range; 650+ / 730+ / 8xx flagship.
QualityTier adreno_tier(int n) {
    if (n < 600) return QualityTier::Low;
    if (n < 700) return n < 620 ? QualityTier::Low : n < 650 ? QualityTier::Medium : QualityTier::High;
    if (n < 800) return n < 730 ? QualityTier::Medium : QualityTier::High;
    return QualityTier::High;
}

} // namespace

DeviceClass classify_device(const DeviceProfile& device, QualityTier fallback) {
    DeviceClass out;
    out.tier = fallback;
    const std::string g = lower(device.gpu);
    std::string what = "unknown GPU";
    if (g.find("llvmpipe") != std::string::npos || g.find("lavapipe") != std::string::npos ||
        g.find("swiftshader") != std::string::npos || g.find("softpipe") != std::string::npos ||
        g.find("microsoft basic render") != std::string::npos) {
        out.family = GpuFamily::Software;
        out.tier = QualityTier::Low;
        what = "software rasterizer";
    } else if (const auto m = g.find("mali-"); m != std::string::npos) {
        out.family = GpuFamily::Mali;
        out.tier = mali_tier(g, m);
        what = "Mali";
    } else if (const auto a = g.find("adreno"); a != std::string::npos) {
        out.family = GpuFamily::Adreno;
        const auto n = number_after(g, a + 6);
        out.tier = n ? adreno_tier(*n) : QualityTier::Low;
        what = n ? std::format("Adreno {}", *n) : "Adreno";
    } else if (g.find("powervr") != std::string::npos || g.find("img bxm") != std::string::npos ||
               g.find("img bxe") != std::string::npos) {
        out.family = GpuFamily::PowerVr;
        out.tier = QualityTier::Low; // the PowerVR parts shipping in phones today are entry level
        what = "PowerVR";
    } else if (g.find("xclipse") != std::string::npos) {
        out.family = GpuFamily::Xclipse;
        out.tier = QualityTier::High;
        what = "Xclipse";
    } else if (g.find("apple") != std::string::npos) {
        out.family = GpuFamily::Apple;
        out.tier = QualityTier::High;
        what = "Apple GPU";
    } else if (g.find("intel") != std::string::npos && g.find(" arc") == std::string::npos) {
        out.family = GpuFamily::IntelIntegrated;
        out.tier = lower_of(fallback, QualityTier::Medium);
        what = "Intel integrated GPU";
    } else if (g.find("nvidia") != std::string::npos || g.find("geforce") != std::string::npos ||
               g.find("radeon") != std::string::npos || g.find("amd") != std::string::npos || g.find(" arc") != std::string::npos) {
        out.family = GpuFamily::Desktop;
        what = "discrete GPU";
    }
    if (out.family == GpuFamily::Mali) {
        // "mali-g52 mc2" -> "Mali-G52"
        const auto m = g.find("mali-");
        std::size_t e = m + 5;
        while (e < g.size() && (std::isalnum(static_cast<unsigned char>(g[e])) != 0)) ++e;
        what = "Mali-" + lower(g.substr(m + 5, e - m - 5));
        for (std::size_t i = 5; i < what.size(); ++i) what[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(what[i])));
    }
    std::string why;
    switch (out.tier) {
    case QualityTier::Low: why = "entry-level"; break;
    case QualityTier::Medium: why = "mid-range"; break;
    case QualityTier::High: why = "high-end"; break;
    case QualityTier::Ultra: why = "high-end"; break;
    }
    out.reason = out.family == GpuFamily::Unknown || out.family == GpuFamily::Desktop
                     ? std::format("{}: project default", what)
                     : std::format("{}: {} GPU", what, why);
    // Memory: textures, lightmaps and shadow maps of a higher tier do not fit next to the OS.
    if (device.ram_mb > 0) {
        const QualityTier cap = device.mobile ? (device.ram_mb < 3072 ? QualityTier::Low : device.ram_mb < 5120 ? QualityTier::Medium : QualityTier::Ultra)
                                              : (device.ram_mb < 4096 ? QualityTier::Medium : QualityTier::Ultra);
        if (static_cast<int>(cap) < static_cast<int>(out.tier)) {
            out.tier = cap;
            out.reason += std::format(", capped by {} MB RAM", device.ram_mb);
        }
    }
    if (!device.compute && static_cast<int>(out.tier) > static_cast<int>(QualityTier::Medium)) {
        out.tier = QualityTier::Medium;
        out.reason += ", capped (no compute shaders)";
    }
    return out;
}

} // namespace victus::render
