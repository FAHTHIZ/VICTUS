// SPDX-License-Identifier: Apache-2.0
// 3D texture uploads (ADR-0115): the SDL backend uploads and reads back the whole volume of a
// Tex3D texture instead of only its first slice, and rejects uploads that are too small or that
// name a mip or layer the texture does not have.
#include "gpu_fixture.hpp"

#include <numeric>

using namespace victus;
using victus::test::gpu;

TEST_CASE("gpu rhi: 3D textures upload and read back every slice") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& dev = gpu().ctx->device();
    rhi::TextureDesc desc;
    desc.name = "volume_test";
    desc.type = rhi::TextureType::Tex3D;
    desc.format = rhi::Format::RGBA8_UNORM;
    desc.width = 4;
    desc.height = 3;
    desc.depth_or_layers = 5;
    auto tex = dev.create_texture(desc);
    REQUIRE(tex.ok());
    std::vector<std::uint8_t> voxels(4 * 3 * 5 * 4);
    for (std::size_t i = 0; i < voxels.size(); ++i) voxels[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xff);
    {
        auto cmd = dev.begin_commands();
        cmd.upload_texture(tex.value(), voxels);
        cmd.submit_and_wait();
    }
    auto back = dev.read_texture(tex.value());
    REQUIRE(back.ok());
    REQUIRE(back.value().size() == voxels.size());
    CHECK(back.value() == voxels); // the last slice too, not just z = 0

    // Too little data, a missing mip or a layer index on a volume are refused: contents stay.
    const std::uint64_t uploads = dev.stats().uploads;
    {
        auto cmd = dev.begin_commands();
        std::vector<std::uint8_t> slice(4 * 3 * 4, 0);
        cmd.upload_texture(tex.value(), slice);             // one slice for a 5-slice volume
        cmd.upload_texture(tex.value(), voxels, 1);         // no mip 1
        cmd.upload_texture(tex.value(), voxels, 0, 2);      // volumes have no layers
        cmd.submit_and_wait();
    }
    CHECK(dev.stats().uploads == uploads);
    CHECK(dev.read_texture(tex.value()).value() == voxels);
    dev.destroy(tex.value());
}

TEST_CASE("gpu rhi: 2D uploads still round-trip") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& dev = gpu().ctx->device();
    rhi::TextureDesc desc;
    desc.name = "array_test";
    desc.type = rhi::TextureType::Tex2D;
    desc.format = rhi::Format::RGBA8_UNORM;
    desc.width = 2;
    desc.height = 2;
    auto tex = dev.create_texture(desc);
    REQUIRE(tex.ok());
    std::vector<std::uint8_t> px(2 * 2 * 4);
    std::iota(px.begin(), px.end(), std::uint8_t{10});
    {
        auto cmd = dev.begin_commands();
        cmd.upload_texture(tex.value(), px);
        cmd.submit_and_wait();
    }
    CHECK(dev.read_texture(tex.value()).value() == px);
    dev.destroy(tex.value());
}
