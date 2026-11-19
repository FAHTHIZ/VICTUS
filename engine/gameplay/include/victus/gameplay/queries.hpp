// SPDX-License-Identifier: Apache-2.0
// Scene geometry queries shared by modules that cannot depend on each other (ADR-0096). Physics
// installs a segment query when a PhysicsWorld is bound to a scene; audio uses it for occlusion
// without linking physics. Without a provider every query reports "no provider".
#pragma once
#include "victus/core/math.hpp"
#include "victus/scene/scene.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace victus::gameplay {

struct SegmentHit {
    scene::Entity entity;
    float distance{0};          // from the segment start
    math::Vec3 normal{0, 1, 0}; // surface normal at the hit (facing the start)
};

// Appends every surface the segment from -> to enters, nearest first, at most `max_hits`.
using SegmentQuery = std::function<void(math::Vec3 from, math::Vec3 to, std::size_t max_hits, std::vector<SegmentHit>& out)>;

// Installs (or with an empty function removes) the scene's segment query provider.
void set_segment_query(scene::Scene& scene, SegmentQuery query);
[[nodiscard]] bool has_segment_query(scene::Scene& scene);
// Runs the provider; returns false (and leaves `out` empty) when none is installed.
bool segment_hits(scene::Scene& scene, math::Vec3 from, math::Vec3 to, std::size_t max_hits, std::vector<SegmentHit>& out);

} // namespace victus::gameplay
