// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/render/device_class.hpp"

using namespace victus::render;

namespace {
DeviceClass phone(const char* gpu, std::uint32_t ram_mb = 8192) {
    return classify_device({gpu, true, ram_mb, true}, QualityTier::Medium);
}
} // namespace

TEST_CASE("device class: phone GPU families map to tiers") {
    // Entry level (the "runs on anything" floor).
    CHECK(phone("vulkan (Mali-G52 MC2, Mali-G52 MC2)").tier == QualityTier::Low);
    CHECK(phone("vulkan (Mali-G57 MC2)").tier == QualityTier::Low);
    CHECK(phone("vulkan (Mali-T830)").tier == QualityTier::Low);
    CHECK(phone("vulkan (Mali-G310)").tier == QualityTier::Low);
    CHECK(phone("vulkan (Adreno (TM) 610)").tier == QualityTier::Low);
    CHECK(phone("vulkan (Adreno (TM) 506)").tier == QualityTier::Low);
    CHECK(phone("vulkan (PowerVR Rogue GE8320)").tier == QualityTier::Low);
    // Mid-range.
    CHECK(phone("vulkan (Mali-G76 MC4)").tier == QualityTier::Medium);
    CHECK(phone("vulkan (Mali-G610 MC6)").tier == QualityTier::Medium);
    CHECK(phone("vulkan (Adreno (TM) 642L)").tier == QualityTier::Medium);
    CHECK(phone("vulkan (Adreno (TM) 710)").tier == QualityTier::Medium);
    // Flagship.
    CHECK(phone("vulkan (Mali-G715 Immortalis MC11)").tier == QualityTier::High);
    CHECK(phone("vulkan (Mali-G720)").tier == QualityTier::High);
    // Names as real drivers report them: Arm's proprietary driver and Mesa PanVK, and Arm's 2025
    // 5th-generation names (Dimensity 9500: "Mali-G1-Ultra MC12").
    CHECK(phone("vulkan (Mali-G720-Immortalis MC12, Mali-G720-Immortalis MC12)").tier == QualityTier::High);
    CHECK(phone("vulkan (Immortalis-G925 MC12, panvk)").tier == QualityTier::High);
    CHECK(phone("vulkan (Immortalis-G925 MC12, panvk)").family == GpuFamily::Mali);
    CHECK(phone("vulkan (Mali-G1-Ultra MC12)").tier == QualityTier::High);
    CHECK(phone("vulkan (Mali-G1-Premium)").tier == QualityTier::High);
    CHECK(phone("vulkan (Mali-G1-Pro)").tier == QualityTier::Medium);
    CHECK(phone("vulkan (Adreno (TM) 660)").tier == QualityTier::High);
    CHECK(phone("vulkan (Adreno (TM) 750)").tier == QualityTier::High);
    CHECK(phone("vulkan (Adreno (TM) 830)").tier == QualityTier::High);
    CHECK(phone("vulkan (Samsung Xclipse 940)").tier == QualityTier::High);
    CHECK(phone("metal (Apple A15 GPU)").tier == QualityTier::High);
    // Unknown GPUs keep the project's phone tier.
    CHECK(phone("vulkan (Some Future GPU)").tier == QualityTier::Medium);
    CHECK(phone("vulkan (Some Future GPU)").family == GpuFamily::Unknown);
}

TEST_CASE("device class: RAM and compute only lower the tier") {
    CHECK(phone("vulkan (Adreno (TM) 750)", 2048).tier == QualityTier::Low);
    CHECK(phone("vulkan (Adreno (TM) 750)", 4096).tier == QualityTier::Medium);
    CHECK(phone("vulkan (Adreno (TM) 750)", 0).tier == QualityTier::High); // unknown RAM: no cap
    CHECK(phone("vulkan (Mali-G52 MC2)", 16384).tier == QualityTier::Low); // RAM never raises it
    const DeviceClass capped = phone("vulkan (Adreno (TM) 750)", 2048);
    CHECK(capped.reason.find("2048 MB") != std::string::npos);
    const DeviceClass no_compute = classify_device({"vulkan (Adreno (TM) 750)", true, 8192, false}, QualityTier::Medium);
    CHECK(no_compute.tier == QualityTier::Medium);
}

TEST_CASE("device class: desktop") {
    CHECK(classify_device({"vulkan (llvmpipe (LLVM 21.1.8, 256 bits), llvmpipe)", false, 16384, true}, QualityTier::High).tier ==
          QualityTier::Low);
    CHECK(classify_device({"vulkan (llvmpipe (LLVM 21.1.8, 256 bits), llvmpipe)", false, 16384, true}, QualityTier::High).family ==
          GpuFamily::Software);
    CHECK(classify_device({"direct3d12 (Microsoft Basic Render Driver)", false, 16384, true}, QualityTier::High).tier ==
          QualityTier::Low);
    CHECK(classify_device({"vulkan (Intel(R) UHD Graphics 620)", false, 8192, true}, QualityTier::High).tier ==
          QualityTier::Medium);
    CHECK(classify_device({"vulkan (Intel(R) Arc(TM) A770 Graphics)", false, 16384, true}, QualityTier::High).tier ==
          QualityTier::High);
    CHECK(classify_device({"vulkan (NVIDIA GeForce RTX 4070)", false, 32768, true}, QualityTier::Ultra).tier == QualityTier::Ultra);
    CHECK(classify_device({"vulkan (NVIDIA GeForce RTX 4070)", false, 2048, true}, QualityTier::Ultra).tier == QualityTier::Medium);
    const DeviceClass mali = phone("vulkan (Mali-G52 MC2, Mali-G52 MC2)");
    CHECK(mali.reason == "Mali-G52: entry-level GPU");
}
