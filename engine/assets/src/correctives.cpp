// SPDX-License-Identifier: Apache-2.0
// Corrective shapes driven by joint angles (ADR-0220). See correctives.hpp.
#include "victus/assets/correctives.hpp"

#include "victus/assets/skeleton.hpp"
#include "victus/render/skinning.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace victus::assets {

namespace {

constexpr float kDeg = 3.14159265358979f / 180.0f;

Error fail(std::string what) { return make_error(ErrorCode::InvalidArgument, std::move(what)); }

// Meshes drawn with `skin` (nodes that reference it), skinned ones only, without duplicates.
std::vector<std::uint32_t> skin_meshes(const CookedModel& m, std::uint32_t skin) {
    std::vector<std::uint32_t> out;
    for (const ModelNode& n : m.nodes)
        if (n.skin == static_cast<std::int32_t>(skin))
            for (const std::uint32_t mi : n.meshes)
                if (mi < m.meshes.size() && m.meshes[mi].data.skinned() && std::find(out.begin(), out.end(), mi) == out.end())
                    out.push_back(mi);
    return out;
}

std::string degrees_text(float d) {
    const float r = std::round(d);
    return std::abs(d - r) < 0.05f ? std::format("{}", static_cast<int>(r)) : std::format("{:.1f}", d);
}

} // namespace

float corrective_angle(math::Quat rest, math::Quat local, math::Vec3 axis) noexcept {
    const float len = math::length(axis);
    if (!(len > 1e-6f)) return 0.0f;
    const math::Vec3 a = axis * (1.0f / len);
    // Rotation relative to the rest pose, in the rest frame: local = rest * d.
    const math::Quat d = math::normalize(rest.conjugate() * local);
    // Twist about `a`: the projection of the vector part onto the axis with the scalar part.
    const float proj = d.x * a.x + d.y * a.y + d.z * a.z;
    float angle = 2.0f * std::atan2(proj, d.w);
    if (angle > 3.14159265f) angle -= 2.0f * 3.14159265f;
    if (angle < -3.14159265f) angle += 2.0f * 3.14159265f;
    return angle;
}

float corrective_weight(const ModelCorrective& c, math::Quat rest, math::Quat local) noexcept {
    if (c.start == c.end) return 0.0f;
    const float a = corrective_angle(rest, local, c.axis);
    const float rise = (a - c.start) / (c.end - c.start); // 1 at end, above 1 past it
    if (rise <= 1.0f) return std::max(rise, 0.0f);
    if (c.release == c.end) return 1.0f;
    return std::clamp(1.0f - (a - c.end) / (c.release - c.end), 0.0f, 1.0f);
}

Result<CorrectiveReport> add_corrective(CookedModel& model, const CorrectiveSettings& s) {
    if (s.skin >= model.skins.size()) return fail(std::format("corrective '{}': the model has no skin {}", s.joint, s.skin));
    const ModelSkin& skin = model.skins[s.skin];
    std::uint32_t joint = 0;
    bool found = false;
    for (std::size_t j = 0; j < skin.joints.size(); ++j)
        if (skin.joints[j] < model.nodes.size() && model.nodes[skin.joints[j]].name == s.joint) {
            joint = static_cast<std::uint32_t>(j);
            found = true;
        }
    if (!found) return fail(std::format("corrective: '{}' is not a joint of skin {}", s.joint, s.skin));
    const float axis_len = math::length(s.axis);
    if (!(axis_len > 1e-4f) || !std::isfinite(axis_len)) return fail(std::format("corrective '{}': zero axis", s.joint));
    if (!std::isfinite(s.degrees) || !std::isfinite(s.start_degrees) || std::abs(s.degrees) > 180.0f || std::abs(s.start_degrees) > 180.0f ||
        std::abs(s.degrees - s.start_degrees) < 1.0f)
        return fail(std::format("corrective '{}': degrees must be within -180..180 and at least 1 degree from start_degrees", s.joint));
    const std::vector<std::uint32_t> meshes = skin_meshes(model, s.skin);
    if (meshes.empty()) return fail(std::format("corrective '{}': no mesh is skinned by skin {}", s.joint, s.skin));

    ModelCorrective c;
    c.joint = joint;
    c.axis = s.axis * (1.0f / axis_len);
    c.start = s.start_degrees * kDeg;
    c.end = s.degrees * kDeg;
    c.release = c.end;
    CorrectiveReport report;
    report.joint = s.joint;

    if (!s.morph.empty()) {
        for (const std::uint32_t mi : meshes)
            for (const render::MorphTarget& t : model.meshes[mi].data.morphs)
                if (t.name == s.morph) ++report.meshes;
        if (report.meshes == 0) return fail(std::format("corrective '{}': no mesh of skin {} has a morph target '{}'", s.joint, s.skin, s.morph));
        c.morph = s.morph;
        report.morph = s.morph;
        report.morphs = {s.morph};
        model.skins[s.skin].correctives.push_back(c);
        return report;
    }

    if (s.target != "dual_quaternion") return fail(std::format("corrective '{}': unknown target '{}' (use dual_quaternion)", s.joint, s.target));
    if (s.steps < 1 || s.steps > 8) return fail(std::format("corrective '{}': steps must be 1..8", s.joint));
    // Key angles: evenly spaced from start to the full angle; shape k rises from key k-1, peaks at
    // key k and (except the last) fades out at key k+1, so neighbours cross-fade linearly.
    std::vector<float> keys(s.steps + 1);
    for (std::uint32_t k = 0; k <= s.steps; ++k) keys[k] = s.start_degrees + (s.degrees - s.start_degrees) * static_cast<float>(k) / static_cast<float>(s.steps);
    std::vector<ModelCorrective> drivers;
    for (std::uint32_t k = 1; k <= s.steps; ++k) {
        ModelCorrective d = c;
        d.morph = std::format("Corrective.{}.{}", s.joint, degrees_text(keys[k]));
        d.start = keys[k - 1] * kDeg;
        d.end = keys[k] * kDeg;
        d.release = k < s.steps ? keys[k + 1] * kDeg : d.end;
        drivers.push_back(d);
    }
    for (const std::uint32_t mi : meshes) {
        const render::MeshData& d = model.meshes[mi].data;
        for (const ModelCorrective& dr : drivers)
            if (std::any_of(d.morphs.begin(), d.morphs.end(), [&](const render::MorphTarget& t) { return t.name == dr.morph; }))
                return fail(std::format("corrective: mesh '{}' already has a morph target '{}'", model.meshes[mi].name, dr.morph));
        if (d.morphs.size() + drivers.size() > render::kMaxMorphTargets)
            return fail(std::format("corrective: mesh '{}' already has {} morph targets", model.meshes[mi].name, d.morphs.size()));
    }

    constexpr float kMinOffset = 1e-5f; // 0.01 mm: below this the methods agree
    const std::uint32_t node = skin.joints[joint];
    // baked[driver][mesh]
    std::vector<std::vector<render::MorphTarget>> baked(drivers.size(), std::vector<render::MorphTarget>(meshes.size()));
    std::vector<render::Vertex3D> lbs, dq, fixed;
    for (std::size_t di = 0; di < drivers.size(); ++di) {
        // Key pose: the rest pose with the joint turned to this key about the axis.
        const std::pair<std::uint32_t, math::Quat> pose[1] = {
            {node, math::normalize(model.nodes[node].rotation * math::Quat::axis_angle(c.axis, drivers[di].end))}};
        const std::vector<math::Mat4> palette = pose_palette(model, pose_world(model, nullptr, 0.0f, pose), s.skin);
        for (std::size_t k = 0; k < meshes.size(); ++k) {
            const render::MeshData& d = model.meshes[meshes[k]].data;
            render::skin_vertices(d, palette, render::SkinningMethod::Linear, lbs);
            render::skin_vertices(d, palette, render::SkinningMethod::DualQuaternion, dq);
            render::MorphTarget& t = baked[di][k];
            t.name = drivers[di].morph;
            for (std::uint32_t i = 0; i < d.vertices.size(); ++i) {
                const math::Vec3 diff = dq[i].position - lbs[i].position;
                if (math::length(diff) < kMinOffset) continue;
                // A morph offset is added before skinning: LBS(v + o) = LBS(v) + M o, so o = M^-1 diff.
                const math::Mat4 inv = math::inverse(render::blend_skin_matrix(d.skin[i], palette, render::SkinningMethod::Linear));
                const math::Vec3 o = inv.transform_vector(diff);
                if (!std::isfinite(o.x + o.y + o.z)) continue;
                // The skinned normal is normalize(M n): pick n along M^-1 n_dq.
                math::Vec3 n = inv.transform_vector(dq[i].normal);
                const float nl = math::length(n);
                n = nl > 1e-8f ? n * (1.0f / nl) : d.vertices[i].normal;
                t.vertices.push_back(i);
                t.positions.push_back(o);
                t.normals.push_back(n - d.vertices[i].normal);
                report.max_offset = std::max(report.max_offset, math::length(o));
            }
            report.vertices += t.vertices.size();
            if (t.vertices.empty()) continue;
            // Residual at the key pose: LBS of the corrected mesh against the target.
            render::MeshData probe = d;
            probe.morphs = {t};
            probe.morph_weights.clear();
            const float one[1] = {1.0f};
            render::apply_morphs(probe, one, fixed);
            render::skin_vertices(d, fixed, palette, render::SkinningMethod::Linear, lbs);
            for (const std::uint32_t i : t.vertices) report.max_residual = std::max(report.max_residual, math::length(lbs[i].position - dq[i].position));
        }
    }
    if (report.vertices == 0)
        return fail(std::format("corrective '{}' at {} degrees: linear and dual quaternion skinning agree, nothing to correct", s.joint,
                                degrees_text(s.degrees)));

    // Commit: append the targets (default weight 0 where the mesh or its nodes list weights) and
    // the drivers of the shapes that moved anything.
    std::vector<std::size_t> appended(meshes.size(), 0);
    for (std::size_t di = 0; di < drivers.size(); ++di) {
        bool any = false;
        for (std::size_t k = 0; k < meshes.size(); ++k) {
            if (baked[di][k].vertices.empty()) continue;
            render::MeshData& d = model.meshes[meshes[k]].data;
            d.morphs.push_back(std::move(baked[di][k]));
            if (!d.morph_weights.empty()) d.morph_weights.push_back(0.0f);
            ++appended[k];
            any = true;
        }
        if (!any) continue;
        model.skins[s.skin].correctives.push_back(drivers[di]);
        report.morphs.push_back(drivers[di].morph);
    }
    for (ModelNode& n : model.nodes) {
        if (n.morph_weights.empty()) continue;
        std::size_t most = 0;
        for (std::size_t k = 0; k < meshes.size(); ++k)
            if (std::find(n.meshes.begin(), n.meshes.end(), meshes[k]) != n.meshes.end()) most = std::max(most, appended[k]);
        n.morph_weights.insert(n.morph_weights.end(), most, 0.0f);
    }
    for (const std::size_t a : appended) report.meshes += a > 0 ? 1 : 0;
    report.baked = true;
    report.morph = report.morphs.back();
    return report;
}

Result<std::vector<CorrectiveReport>> apply_import_correctives(CookedModel& model, const ModelImportSettings& settings) {
    std::vector<CorrectiveReport> out;
    for (const CorrectiveSettings& s : settings.correctives) {
        auto r = add_corrective(model, s);
        if (!r) return std::move(r).error();
        out.push_back(std::move(*r));
    }
    return out;
}

} // namespace victus::assets
