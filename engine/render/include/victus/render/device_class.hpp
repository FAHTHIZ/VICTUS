// SPDX-License-Identifier: Apache-2.0
// Device-class quality selection (ADR-0127): picks a starting quality tier from what the player
// can see about the device (GPU name, phone or not, RAM, compute support), the way Unity's
// Android quality presets and Unreal's device profiles do, so `rendering.quality = "auto"` runs
// at Low on an entry-level Mali and at High on a flagship Adreno without per-game tuning.
#pragma once

#include "victus/render/scene_renderer.hpp"

#include <cstdint>
#include <string>

namespace victus::render {

struct DeviceProfile {
    std::string gpu;            // rhi::DeviceCapabilities::driver_name, e.g. "vulkan (Mali-G52 MC2, ...)"
    bool mobile{false};         // Android or iOS
    std::uint32_t ram_mb{0};    // system RAM; 0 = unknown
    bool compute{true};         // compute pipelines (SSAO, occlusion culling, GPU particles)
};

// The GPU families the classifier knows. Unknown keeps the platform default.
enum class GpuFamily : std::uint8_t { Unknown, Software, Mali, Adreno, PowerVr, Xclipse, Apple, IntelIntegrated, Desktop };

struct DeviceClass {
    QualityTier tier{QualityTier::High};
    GpuFamily family{GpuFamily::Unknown};
    std::string reason; // one line for logs and `victus run`, e.g. "Mali-G52: entry-level GPU"
};

// `fallback` is used when nothing about the device says otherwise (the project's phone or desktop
// tier). RAM and missing compute only ever lower the result.
[[nodiscard]] DeviceClass classify_device(const DeviceProfile& device, QualityTier fallback);

} // namespace victus::render
