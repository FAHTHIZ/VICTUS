// SPDX-License-Identifier: BSD-3-Clause AND Apache-2.0
// Single translation unit hosting NVIDIA's FLIP.h (BSD-3-Clause, third_party/flip/LICENSE),
// vendored from https://github.com/NVlabs/flip at d3ab02a4425eb167d8a4021b382a2bafc6e6c838.
// Compiled without VICTUS warning flags; victus/render/image_compare.cpp calls these wrappers.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "FLIP.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <vector>

namespace victus_flip {

// LDR-FLIP of two linear-RGB images in [0, 1] (3 floats per pixel), at the given pixels per degree.
std::vector<float> ldr_error_map(const float* reference, const float* test, int width, int height, float ppd) {
    FLIP::Parameters parameters;
    parameters.PPD = ppd;
    FLIP::image<FLIP::color3> ref, tst;
    ref.setPixels(reference, width, height);
    tst.setPixels(test, width, height);
    FLIP::image<float> map(width, height, 0.0f);
    FLIP::evaluate(ref, tst, false, parameters, map);
    std::vector<float> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) out[static_cast<std::size_t>(y) * width + x] = map.get(x, y);
    return out;
}

float pixels_per_degree(float distance_m, float resolution_x, float width_m) {
    return FLIP::calculatePPD(distance_m, resolution_x, width_m);
}

// FLIP's magma colour map (sRGB in [0, 1]) for an error value in [0, 1].
void magma(float value, float rgb[3]) {
    const int i = value <= 0.0f ? 0 : value >= 1.0f ? 255 : static_cast<int>(value * 255.0f + 0.5f);
    const FLIP::color3 c = FLIP::MapMagma[i];
    rgb[0] = c.x;
    rgb[1] = c.y;
    rgb[2] = c.z;
}

} // namespace victus_flip
