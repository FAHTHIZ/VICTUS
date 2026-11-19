// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/queries.hpp"

#include <utility>

namespace victus::gameplay {

namespace {
struct SegmentQueryProvider {
    SegmentQuery query;
};
} // namespace

void set_segment_query(scene::Scene& scene, SegmentQuery query) {
    if (auto* p = scene.world().resource<SegmentQueryProvider>()) p->query = std::move(query);
    else scene.world().set_resource<SegmentQueryProvider>().query = std::move(query);
}

bool has_segment_query(scene::Scene& scene) {
    const auto* p = scene.world().resource<SegmentQueryProvider>();
    return p && p->query;
}

bool segment_hits(scene::Scene& scene, math::Vec3 from, math::Vec3 to, std::size_t max_hits, std::vector<SegmentHit>& out) {
    out.clear();
    const auto* p = scene.world().resource<SegmentQueryProvider>();
    if (!p || !p->query) return false;
    if (max_hits > 0) p->query(from, to, max_hits, out);
    return true;
}

} // namespace victus::gameplay
