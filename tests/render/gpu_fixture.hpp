// SPDX-License-Identifier: Apache-2.0
// Process-wide headless GPU for tests. Tests skip (with a message) when no device exists,
// unless VICTUS_REQUIRE_GPU=1, in which case they fail so CI cannot silently skip coverage.
#pragma once
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/platform/platform.hpp"
#include "victus/render/gpu_context.hpp"
#include "victus/rhi/rhi.hpp"

#include <cstdlib>
#include <memory>

namespace victus::test {

struct Gpu {
    std::unique_ptr<platform::Platform> platform;
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::GpuContext> ctx;
    std::string failure;
};

Gpu& gpu();
bool gpu_required();
// Writes an RGBA8 readback to <build>/test_output/<name>.png for visual inspection.
void save_output(const char* name, std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba);
// RGBA8 colour target that can be read back.
rhi::TextureHandle make_target(std::uint32_t w, std::uint32_t h, rhi::Format f = rhi::Format::RGBA8_UNORM);
std::uint32_t pixel(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t x, std::uint32_t y);
inline std::uint8_t channel(std::uint32_t px, int c) { return static_cast<std::uint8_t>((px >> (c * 8)) & 0xff); }

} // namespace victus::test

#define VICTUS_REQUIRE_GPU_OR_SKIP()                                                                 \
    do {                                                                                           \
        if (!::victus::test::gpu().ctx) {                                                          \
            if (::victus::test::gpu_required()) FAIL("GPU required but unavailable: " << ::victus::test::gpu().failure); \
            MESSAGE("skipping GPU test: " << ::victus::test::gpu().failure);                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)
