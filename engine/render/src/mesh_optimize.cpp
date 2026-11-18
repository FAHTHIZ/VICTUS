// SPDX-License-Identifier: Apache-2.0
// Mesh optimisation and LOD generation on top of meshoptimizer (ADR-0049).
#include "victus/render/mesh.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>

namespace victus::render {

namespace {
constexpr std::size_t kStride = sizeof(Vertex3D);
const float* positions(const MeshData& m) { return &m.vertices.data()->position.x; }
} // namespace

void optimize_mesh(MeshData& mesh) {
    const std::size_t vc = mesh.vertices.size();
    if (vc == 0 || mesh.indices.size() < 3) return;
    auto cache_order = [&](std::vector<std::uint32_t>& idx) {
        if (idx.size() < 3) return;
        meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(), vc);
    };
    cache_order(mesh.indices);
    // Overdraw: allow up to 5% more vertex cache misses for a better front-to-back triangle order.
    meshopt_optimizeOverdraw(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), positions(mesh), vc, kStride,
                             1.05f);
    for (MeshLod& lod : mesh.lods) cache_order(lod.indices);

    // Vertex fetch: renumber vertices in first-use order (LOD 0 first, then vertices only coarser
    // levels use, which simplification never creates, so in practice none).
    std::vector<std::uint32_t> remap(vc, ~0u);
    std::uint32_t next = 0;
    auto visit = [&](const std::vector<std::uint32_t>& idx) {
        for (const std::uint32_t i : idx)
            if (remap[i] == ~0u) remap[i] = next++;
    };
    visit(mesh.indices);
    for (const MeshLod& lod : mesh.lods) visit(lod.indices);
    // Unreferenced vertices are dropped.
    std::vector<Vertex3D> vertices(next);
    std::vector<SkinVertex> skin(mesh.skinned() ? next : 0);
    for (std::size_t v = 0; v < vc; ++v) {
        if (remap[v] == ~0u) continue;
        vertices[remap[v]] = mesh.vertices[v];
        if (!skin.empty()) skin[remap[v]] = mesh.skin[v];
    }
    mesh.vertices = std::move(vertices);
    if (!skin.empty()) mesh.skin = std::move(skin);
    for (std::uint32_t& i : mesh.indices) i = remap[i];
    for (MeshLod& lod : mesh.lods)
        for (std::uint32_t& i : lod.indices) i = remap[i];
    // Morph targets follow the renumbering (entries of dropped vertices go away) and stay sorted.
    for (MorphTarget& t : mesh.morphs) {
        std::vector<std::size_t> order;
        order.reserve(t.vertices.size());
        for (std::size_t k = 0; k < t.vertices.size(); ++k)
            if (t.vertices[k] < vc && remap[t.vertices[k]] != ~0u) order.push_back(k);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return remap[t.vertices[a]] < remap[t.vertices[b]]; });
        MorphTarget out{.name = std::move(t.name), .vertices = {}, .positions = {}, .normals = {}};
        for (const std::size_t k : order) {
            out.vertices.push_back(remap[t.vertices[k]]);
            out.positions.push_back(t.positions[k]);
            if (!t.normals.empty()) out.normals.push_back(t.normals[k]);
        }
        t = std::move(out);
    }
}

std::uint32_t generate_lods(MeshData& mesh, const LodSettings& settings) {
    mesh.lods.clear();
    const std::size_t vc = mesh.vertices.size();
    const std::size_t tris = mesh.indices.size() / 3;
    const std::uint32_t max_lods = std::min(settings.max_lods, kMaxMeshLods - 1);
    if (vc == 0 || tris == 0 || max_lods == 0) return 0;
    const float ratio = std::clamp(settings.ratio, 0.05f, 0.95f);
    const float max_error = std::clamp(settings.max_error, 1e-4f, 1.0f);
    const float scale = meshopt_simplifyScale(positions(mesh), vc, kStride);
    if (!(scale > 0.0f)) return 0;

    // Attributes keep shading and texturing intact: normals (weight 0.5) and UVs (weight 0.2 per
    // unit of UV space). Skinned meshes add the dominant joint (weight 1) so edges do not collapse
    // across bone boundaries, where the simplified surface would tear apart when animated.
    const bool skinned = mesh.skinned();
    const std::size_t stride = skinned ? 6 : 5;
    std::vector<float> attributes(vc * stride);
    for (std::size_t v = 0; v < vc; ++v) {
        const Vertex3D& x = mesh.vertices[v];
        float* a = &attributes[v * stride];
        a[0] = x.normal.x;
        a[1] = x.normal.y;
        a[2] = x.normal.z;
        a[3] = x.uv.x;
        a[4] = x.uv.y;
        if (skinned) {
            const SkinVertex& sv = mesh.skin[v];
            int best = 0;
            for (int k = 1; k < 4; ++k)
                if (sv.weights[k] > sv.weights[best]) best = k;
            a[5] = static_cast<float>(sv.joints[best]);
        }
    }
    const float weights[6] = {0.5f, 0.5f, 0.5f, 0.2f, 0.2f, 1.0f};
    const unsigned options = settings.lock_borders ? static_cast<unsigned>(meshopt_SimplifyLockBorder) : 0u;

    std::size_t previous = mesh.indices.size();
    float previous_error = 0.0f;
    float budget = 1.0f;
    std::vector<std::uint32_t> out(mesh.indices.size());
    for (std::uint32_t level = 0; level < max_lods; ++level) {
        budget *= ratio;
        const std::size_t target = static_cast<std::size_t>(std::max(1.0f, std::floor(static_cast<float>(tris) * budget))) * 3;
        if (target / 3 < settings.min_triangles) break;
        float error = 0.0f;
        // Simplifying from the full mesh (not the previous level) keeps errors from accumulating.
        const std::size_t count = meshopt_simplifyWithAttributes(
            out.data(), mesh.indices.data(), mesh.indices.size(), positions(mesh), vc, kStride, attributes.data(),
            stride * sizeof(float), weights, stride, nullptr, target, max_error, options, &error);
        // Not worth a level: under 10% fewer triangles than the previous one.
        if (count == 0 || static_cast<double>(count) > 0.9 * static_cast<double>(previous)) break;
        if (count / 3 < settings.min_triangles) break;
        MeshLod lod;
        lod.indices.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count));
        meshopt_optimizeVertexCache(lod.indices.data(), lod.indices.data(), lod.indices.size(), vc);
        lod.error = std::max(error * scale, previous_error);
        previous_error = lod.error;
        previous = count;
        mesh.lods.push_back(std::move(lod));
    }
    return static_cast<std::uint32_t>(mesh.lods.size());
}

} // namespace victus::render
