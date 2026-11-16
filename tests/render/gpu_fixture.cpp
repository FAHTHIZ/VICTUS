// SPDX-License-Identifier: Apache-2.0
#include "gpu_fixture.hpp"

#include "victus/render/image.hpp"

#include <cstring>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace victus::test {

bool gpu_required() {
    const char* v = std::getenv("VICTUS_REQUIRE_GPU");
    return v && v[0] == '1';
}

Gpu& gpu() {
    static Gpu g = [] {
        Gpu r;
        if (const char* skip = std::getenv("VICTUS_NO_GPU"); skip && skip[0] == '1') {
            r.failure = "VICTUS_NO_GPU=1";
            return r;
        }
#if !defined(_WIN32) && !defined(__APPLE__)
        // Headless Linux: SDL's offscreen video driver still allows GPU device creation.
        if (!std::getenv("SDL_VIDEO_DRIVER") && !std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
            setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
#endif
        platform::PlatformDesc pd;
        pd.gamepad = false;
        auto p = platform::Platform::create(pd);
        if (!p) {
            r.failure = p.error().message;
            return r;
        }
        r.platform = std::move(p).value();
        rhi::DeviceDesc dd;
        dd.headless = true;
        dd.debug = false;
        auto d = rhi::Device::create(dd);
        if (!d) {
            r.failure = d.error().message;
            return r;
        }
        r.device = std::move(d).value();
        auto c = render::GpuContext::create(*r.device);
        if (!c) {
            r.failure = c.error().message;
            return r;
        }
        r.ctx = std::move(c).value();
        MESSAGE("GPU: " << r.device->capabilities().driver_name);
        return r;
    }();
    return g;
}

void save_output(const char* name, std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba) {
    render::Image img;
    img.width = w;
    img.height = h;
    img.pixels = rgba;
    for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
    const fs::Path dir = fs::Path(VICTUS_TEST_OUTPUT_DIR);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    (void)render::save_png(dir / (std::string(name) + ".png"), img);
}

rhi::TextureHandle make_target(std::uint32_t w, std::uint32_t h, rhi::Format f) {
    rhi::TextureDesc d;
    d.name = "test_target";
    d.format = f;
    d.width = w;
    d.height = h;
    d.usage = rhi::TextureUsageColorTarget | rhi::TextureUsageSampled;
    auto t = gpu().device->create_texture(d);
    REQUIRE(t.ok());
    return t.value();
}

std::uint32_t pixel(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t x, std::uint32_t y) {
    std::uint32_t v;
    std::memcpy(&v, rgba.data() + (std::size_t{y} * w + x) * 4, 4);
    return v;
}

} // namespace victus::test
