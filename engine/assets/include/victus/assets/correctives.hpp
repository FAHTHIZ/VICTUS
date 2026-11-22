// SPDX-License-Identifier: Apache-2.0
#pragma once
// Corrective shapes driven by joint angles (pose-space deformation, ADR-0220).
//
// A corrective is a morph target whose weight follows how far one joint has turned about one axis.
// Shapes can be authored (sculpted in Blender and exported as a shape key) or baked here: the
// per-vertex offsets that make linear blend skinning reproduce dual quaternion skinning at a key
// pose, so LBS meshes keep their volume where a forearm twists or a knee bends, without changing
// the skinning method of the whole mesh. Drivers are stored on the skin (ModelSkin::correctives)
// and evaluated by the runtime renderer from the posed skeleton every frame.
#include "victus/assets/cooked.hpp"
#include "victus/assets/importers.hpp"

#include <string>
#include <vector>

namespace victus::assets {

// Signed angle (radians, -pi..pi) of `local`'s rotation away from `rest` about `axis`, measured in
// the rest frame (swing-twist decomposition: rotation about other axes does not count).
[[nodiscard]] float corrective_angle(math::Quat rest, math::Quat local, math::Vec3 axis) noexcept;
// The driver's weight: linear from 0 at c.start to 1 at c.end, then held or faded out by c.release.
[[nodiscard]] float corrective_weight(const ModelCorrective& c, math::Quat rest, math::Quat local) noexcept;

struct CorrectiveReport {
    std::string morph;               // the full-angle shape (or the driven one)
    std::vector<std::string> morphs; // every shape added, in-betweens first
    std::string joint;
    bool baked{false};            // false: an existing morph target is driven
    std::size_t meshes{0};        // meshes that carry the target
    std::size_t vertices{0};      // vertices moved by the baked target
    float max_offset{0.0f};       // largest baked offset (bind space, metres)
    float max_residual{0.0f};     // largest distance from the target method at the key pose after baking
};

// Bakes or binds one corrective on `model` and appends its driver to the skin. Fails (leaving the
// model unchanged) when the joint is not a skin joint, the axis or range is degenerate, the morph
// to drive exists on no mesh of the skin, a baked shape would be empty, or the name is taken.
[[nodiscard]] Result<CorrectiveReport> add_corrective(CookedModel& model, const CorrectiveSettings& settings);
// Import hook: applies `settings.correctives` in order; the first failure fails the import.
[[nodiscard]] Result<std::vector<CorrectiveReport>> apply_import_correctives(CookedModel& model, const ModelImportSettings& settings);

// Morph weights of one mesh with every corrective of `skin` that targets it added in: `base`
// (one per target, missing = 0) plus each driver's weight at the joints' current local rotations
// (`local_rotation(node)` returns the posed local rotation of a model node). Returns the number of
// drivers with a non-zero weight.
template <class LocalRotation>
std::size_t corrective_mesh_weights(const CookedModel& model, const ModelSkin& skin, const render::MeshData& mesh,
                                    std::span<const float> base, LocalRotation&& local_rotation, std::vector<float>& out) {
    out.assign(mesh.morphs.size(), 0.0f);
    for (std::size_t i = 0; i < out.size() && i < base.size(); ++i) out[i] = base[i];
    std::size_t active = 0;
    for (const ModelCorrective& c : skin.correctives) {
        if (c.joint >= skin.joints.size()) continue;
        const std::uint32_t node = skin.joints[c.joint];
        if (node >= model.nodes.size()) continue;
        for (std::size_t t = 0; t < mesh.morphs.size(); ++t) {
            if (mesh.morphs[t].name != c.morph) continue;
            const float w = corrective_weight(c, model.nodes[node].rotation, local_rotation(node));
            out[t] += w;
            active += w > 0.0f ? 1 : 0;
            break;
        }
    }
    return active;
}

} // namespace victus::assets
