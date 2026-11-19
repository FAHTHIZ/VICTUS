// SPDX-License-Identifier: Apache-2.0
// World streaming (6h) on V.A.U.L.T. (6g): cells request the textures, models and material
// textures they reference through the GPU asset cache, and activate once those are resident.
#pragma once
#include "victus/runtime/scene_rendering.hpp"
#include "victus/world/world.hpp"

namespace victus::runtime {

// Dependencies backed by `cache` (and its asset database): textures and models are prefetched
// at Normal priority, materials prefetch their textures; anything else is ready at once. With
// synchronous loading every dependency is ready (it loads on first use).
[[nodiscard]] world::Dependencies world_dependencies(GpuAssetCache& cache);

} // namespace victus::runtime
