// SPDX-License-Identifier: Apache-2.0
#include "victus/render/reflection_capture.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>

namespace victus::render {
namespace {
struct FaceBasis {
    math::Vec3 forward, up;
};
constexpr std::array<FaceBasis, 6> kFaces{{
    {{1, 0, 0}, {0, 1, 0}},
    {{-1, 0, 0}, {0, 1, 0}},
    {{0, 1, 0}, {0, 0, 1}},
    {{0, -1, 0}, {0, 0, -1}},
    {{0, 0, 1}, {0, 1, 0}},
    {{0, 0, -1}, {0, 1, 0}},
}};
math::Vec3 face_right(const FaceBasis& b) { return math::normalize(math::cross(b.forward, b.up)); }

math::Vec4 bilinear(const ImageF& img, float x, float y) {
    x = std::clamp(x - 0.5f, 0.0f, static_cast<float>(img.width - 1));
    y = std::clamp(y - 0.5f, 0.0f, static_cast<float>(img.height - 1));
    const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
    const std::uint32_t x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const auto lerp = [](math::Vec4 a, math::Vec4 b, float t) { return a + (b - a) * t; };
    return lerp(lerp(img.at(x0, y0), img.at(x1, y0), fx), lerp(img.at(x0, y1), img.at(x1, y1), fx), fy);
}
} // namespace

Camera3D capture_face_camera(math::Vec3 position, int face, float z_near, float z_far) {
    const FaceBasis& b = kFaces[static_cast<std::size_t>(std::clamp(face, 0, 5))];
    Camera3D c = Camera3D::look_at(position, position + b.forward, b.up);
    c.fov_y = math::radians(90.0f);
    c.z_near = z_near;
    c.z_far = z_far;
    return c;
}

ImageF faces_to_equirect(const std::array<ImageF, 6>& faces, std::uint32_t width) {
    ImageF out;
    out.width = std::max(4u, width & ~1u);
    out.height = out.width / 2;
    out.pixels.resize(std::size_t{out.width} * out.height * 4);
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const math::Vec3 d = equirect_direction((static_cast<float>(x) + 0.5f) / static_cast<float>(out.width),
                                                    (static_cast<float>(y) + 0.5f) / static_cast<float>(out.height));
            std::size_t best = 0;
            float best_dot = -2.0f;
            for (std::size_t f = 0; f < 6; ++f) {
                const float k = math::dot(d, kFaces[f].forward);
                if (k > best_dot) {
                    best_dot = k;
                    best = f;
                }
            }
            const ImageF& img = faces[best];
            math::Vec4 c{0, 0, 0, 1};
            if (!img.empty()) {
                const float sx = math::dot(d, face_right(kFaces[best])) / best_dot;
                const float sy = math::dot(d, kFaces[best].up) / best_dot;
                c = bilinear(img, (sx * 0.5f + 0.5f) * static_cast<float>(img.width),
                             (0.5f - sy * 0.5f) * static_cast<float>(img.height));
            }
            float* p = &out.pixels[(std::size_t{y} * out.width + x) * 4];
            p[0] = c.x;
            p[1] = c.y;
            p[2] = c.z;
            p[3] = 1.0f;
        }
    return out;
}

Result<ImageF> capture_equirect(GpuContext& ctx, SceneRenderer& renderer, math::Vec3 position, std::uint32_t face_size,
                                const CaptureFill& fill, float z_near, float z_far) {
    VICTUS_PROFILE_SCOPE("capture_equirect");
    auto& dev = ctx.device();
    const std::uint32_t size = std::clamp(face_size, 16u, 1024u);
    rhi::TextureDesc td;
    td.name = "probe_capture_face";
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width = td.height = size;
    td.usage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
    auto target = dev.create_texture(td);
    if (!target) return std::move(target).error();
    const RenderSettings saved = renderer.settings();
    RenderSettings capture = saved;
    capture.fxaa = false;
    capture.bloom = false;
    capture.auto_exposure = false;
    capture.render_scale = 1.0f;
    renderer.set_settings(capture);
    renderer.set_capture_mode(true);
    std::array<ImageF, 6> faces;
    Status status;
    for (int f = 0; f < 6 && status.ok(); ++f) {
        fill(renderer, capture_face_camera(position, f, z_near, z_far));
        auto cmd = dev.begin_commands();
        status = renderer.render(cmd, *target, size, size, rhi::Format::RGBA16_FLOAT);
        cmd.submit_and_wait();
        if (!status.ok()) break;
        auto px = dev.read_texture(*target);
        if (!px) {
            status = std::move(px).error();
            break;
        }
        if (px->size() < std::size_t{size} * size * 8) {
            status = make_error(ErrorCode::Unknown, "probe capture: short readback");
            break;
        }
        ImageF& img = faces[static_cast<std::size_t>(f)];
        img.width = img.height = size;
        img.pixels.resize(std::size_t{size} * size * 4);
        const auto* h = reinterpret_cast<const std::uint8_t*>(px->data());
        for (std::size_t i = 0; i < img.pixels.size(); ++i)
            img.pixels[i] = half_to_float(static_cast<std::uint16_t>(h[i * 2] | (h[i * 2 + 1] << 8)));
    }
    renderer.set_capture_mode(false);
    renderer.set_settings(saved);
    dev.destroy(*target);
    if (!status.ok()) return status.error();
    return faces_to_equirect(faces, size * 4);
}

} // namespace victus::render
