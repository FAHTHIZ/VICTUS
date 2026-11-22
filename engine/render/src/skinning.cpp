// SPDX-License-Identifier: Apache-2.0
// CPU skinning reference, dual quaternion encoding, weight tools and deformation metrics
// (skinning.hpp, ADR-0205).
#include "victus/render/skinning.hpp"

#include <algorithm>
#include <cmath>

namespace victus::render {

using math::Mat4;
using math::Quat;
using math::Vec3;
using math::Vec4;

std::string_view skinning_method_name(SkinningMethod m) noexcept {
    return m == SkinningMethod::DualQuaternion ? "dual_quaternion" : "linear";
}

namespace {

Quat quat_from_basis(Vec3 x, Vec3 y, Vec3 z) noexcept {
    // Shepperd's method on the orthonormal basis (columns x, y, z).
    const float tr = x.x + y.y + z.z;
    Quat q;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return math::normalize(q);
}

Quat qmul(Quat a, Quat b) noexcept { return a * b; }
Quat qscale(Quat q, float s) noexcept { return {q.x * s, q.y * s, q.z * s, q.w * s}; }
Quat qadd(Quat a, Quat b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
float qdot(Quat a, Quat b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Mat4 lerp_add(const Mat4& acc, const Mat4& m, float w) noexcept {
    Mat4 r = acc;
    for (int c = 0; c < 4; ++c) r.c[c] = r.c[c] + m.c[c] * w;
    return r;
}

std::uint32_t pack(float r, float g, float b) noexcept {
    const auto ch = [](float v) { return static_cast<std::uint32_t>(std::lround(math::saturate(v) * 255.0f)); };
    return ch(r) | (ch(g) << 8) | (ch(b) << 16) | (0xffu << 24);
}

} // namespace

DualQuat to_dual_quat(const Mat4& m, Vec3* scale) noexcept {
    Vec3 x = m.c[0].xyz(), y = m.c[1].xyz(), z = m.c[2].xyz();
    Vec3 s{math::length(x), math::length(y), math::length(z)};
    // Gram-Schmidt: removes shear so the rotation is the nearest orthonormal frame.
    x = s.x > 0 ? x / s.x : Vec3{1, 0, 0};
    y = y - x * math::dot(x, y);
    const float ly = math::length(y);
    y = ly > 1e-12f ? y / ly : math::normalize(math::cross(Vec3{0, 0, 1}, x));
    Vec3 zz = math::cross(x, y);
    if (math::dot(zz, z) < 0.0f) s.z = -s.z; // mirrored joint: keep handedness in the scale
    z = zz;
    if (scale) *scale = s;
    DualQuat dq;
    dq.real = quat_from_basis(x, y, z);
    const Vec3 t = m.c[3].xyz();
    dq.dual = qscale(qmul(Quat{t.x, t.y, t.z, 0.0f}, dq.real), 0.5f);
    return dq;
}

Mat4 to_matrix(const DualQuat& in, Vec3 scale) noexcept {
    const float len = std::sqrt(qdot(in.real, in.real));
    const Quat r = len > 0 ? qscale(in.real, 1.0f / len) : Quat{};
    const Quat d = len > 0 ? qscale(in.dual, 1.0f / len) : Quat{0, 0, 0, 0};
    const Vec3 rv{r.x, r.y, r.z}, dv{d.x, d.y, d.z};
    const Vec3 t = (dv * r.w - rv * d.w + math::cross(rv, dv)) * 2.0f;
    Mat4 m = Mat4::rotation(r);
    m.c[0] = m.c[0] * scale.x;
    m.c[1] = m.c[1] * scale.y;
    m.c[2] = m.c[2] * scale.z;
    m.c[3] = {t, 1.0f};
    return m;
}

Mat4 encode_dq_joint(const Mat4& skin) noexcept {
    Vec3 s;
    const DualQuat dq = to_dual_quat(skin, &s);
    Mat4 m;
    m.c[0] = {dq.real.x, dq.real.y, dq.real.z, dq.real.w};
    m.c[1] = {dq.dual.x, dq.dual.y, dq.dual.z, dq.dual.w};
    m.c[2] = {s, 0.0f};
    m.c[3] = {0, 0, 0, -1};
    return m;
}

void encode_dq_palette(std::span<Mat4> palette) noexcept {
    for (Mat4& m : palette)
        if (!is_dq_joint(m)) m = encode_dq_joint(m);
}

Mat4 blend_skin_matrix(const SkinVertex& v, std::span<const Mat4> palette, SkinningMethod method) noexcept {
    const auto joint = [&](std::uint16_t j) -> const Mat4& {
        static const Mat4 identity{};
        return j < palette.size() ? palette[j] : identity;
    };
    if (method == SkinningMethod::Linear) {
        Mat4 acc;
        for (int c = 0; c < 4; ++c) acc.c[c] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (v.weights[k] != 0.0f) acc = lerp_add(acc, joint(v.joints[k]), v.weights[k]);
        return acc;
    }
    // DQS with antipodality fix against the first influence (as the shaders do).
    Vec3 s0;
    const DualQuat first = to_dual_quat(joint(v.joints[0]), &s0);
    DualQuat acc{qscale(first.real, v.weights[0]), qscale(first.dual, v.weights[0])};
    Vec3 scale = s0 * v.weights[0];
    for (int k = 1; k < 4; ++k) {
        if (v.weights[k] == 0.0f) continue;
        Vec3 sk;
        const DualQuat dq = to_dual_quat(joint(v.joints[k]), &sk);
        const float w = qdot(first.real, dq.real) < 0.0f ? -v.weights[k] : v.weights[k];
        acc.real = qadd(acc.real, qscale(dq.real, w));
        acc.dual = qadd(acc.dual, qscale(dq.dual, w));
        scale = scale + sk * v.weights[k];
    }
    return to_matrix(acc, scale);
}

void skin_vertices(const MeshData& mesh, std::span<const Vertex3D> base, std::span<const Mat4> palette,
                   SkinningMethod method, std::vector<Vertex3D>& out) {
    out.assign(base.begin(), base.end());
    if (!mesh.skinned() || base.size() != mesh.skin.size()) return;
    for (std::size_t i = 0; i < base.size(); ++i) {
        const Mat4 m = blend_skin_matrix(mesh.skin[i], palette, method);
        Vertex3D& o = out[i];
        o.position = m.transform_point(base[i].position);
        o.normal = math::normalize(m.transform_vector(base[i].normal));
        const Vec3 t = math::normalize(m.transform_vector(base[i].tangent.xyz()));
        o.tangent = {t, base[i].tangent.w};
    }
}

// ---- weights -----------------------------------------------------------------------------------

std::size_t SkinWeightReport::unweighted_joints() const noexcept {
    return static_cast<std::size_t>(std::count(joint_vertices.begin(), joint_vertices.end(), 0u));
}

SkinWeightReport analyze_skin_weights(const MeshData& mesh, std::size_t joint_count) {
    SkinWeightReport r;
    r.joint_vertices.assign(joint_count, 0);
    r.joint_weight.assign(joint_count, 0.0f);
    if (!mesh.skinned()) return r;
    r.vertices = mesh.skin.size();
    for (const SkinVertex& v : mesh.skin) {
        float sum = 0;
        std::size_t n = 0;
        bool range_error = false;
        for (int k = 0; k < 4; ++k) {
            const float w = v.weights[k];
            if (!(w > 0.0f)) continue;
            sum += w;
            ++n;
            if (w < kTinyWeight) ++r.tiny_influences;
            if (v.joints[k] >= joint_count) {
                range_error = true;
                continue;
            }
            ++r.joint_vertices[v.joints[k]];
            r.joint_weight[v.joints[k]] += w;
        }
        if (range_error) ++r.out_of_range;
        ++r.influence_histogram[n];
        if (n == 0) ++r.zero_weight;
        const float err = std::fabs(sum - 1.0f);
        r.max_weight_error = std::max(r.max_weight_error, err);
        if (err > 1e-3f) ++r.unnormalized;
    }
    return r;
}

std::size_t normalize_skin_weights(MeshData& mesh) noexcept {
    std::size_t changed = 0;
    for (SkinVertex& v : mesh.skin) {
        float sum = 0;
        for (float& w : v.weights) {
            if (!(w > 0.0f) || !std::isfinite(w)) w = 0.0f;
            sum += w;
        }
        if (!(sum > 0.0f)) {
            v = SkinVertex{};
            ++changed;
            continue;
        }
        if (std::fabs(sum - 1.0f) <= 1e-6f) continue;
        for (float& w : v.weights) w /= sum;
        ++changed;
    }
    return changed;
}

std::size_t limit_skin_influences(MeshData& mesh, std::uint32_t max_influences, float min_weight) noexcept {
    max_influences = std::clamp<std::uint32_t>(max_influences, 1, 4);
    std::size_t changed = 0;
    for (SkinVertex& v : mesh.skin) {
        std::uint32_t j[4];
        float w[4];
        for (int k = 0; k < 4; ++k) {
            j[k] = v.joints[k];
            w[k] = v.weights[k];
        }
        // make_skin_vertex sorts by descending weight; re-run it on the pruned set.
        bool dirty = false;
        std::uint32_t kept = 0;
        float order[4] = {w[0], w[1], w[2], w[3]};
        std::sort(order, order + 4, std::greater<>());
        const float cut = max_influences < 4 ? order[max_influences] : -1.0f;
        for (int k = 0; k < 4; ++k) {
            const bool drop = (w[k] > 0.0f && w[k] < min_weight) || (w[k] > 0.0f && w[k] <= cut && kept >= max_influences);
            if (drop) {
                w[k] = 0.0f;
                dirty = true;
            } else if (w[k] > 0.0f) {
                ++kept;
            }
        }
        // Too many equal weights at the cut: drop the extras from the end.
        for (int k = 3; k >= 0 && kept > max_influences; --k)
            if (w[k] > 0.0f && w[k] <= cut) {
                w[k] = 0.0f;
                --kept;
                dirty = true;
            }
        if (!dirty) continue;
        if (kept == 0) { // everything was tiny: keep the original strongest influence
            const int best = static_cast<int>(std::max_element(v.weights, v.weights + 4) - v.weights);
            w[best] = 1.0f;
        }
        v = make_skin_vertex(j, w, 4);
        ++changed;
    }
    return changed;
}

std::vector<float> joint_weights(const MeshData& mesh, std::uint32_t joint) {
    std::vector<float> out(mesh.vertices.size(), 0.0f);
    if (!mesh.skinned()) return out;
    for (std::size_t i = 0; i < mesh.skin.size(); ++i)
        for (int k = 0; k < 4; ++k)
            if (mesh.skin[i].joints[k] == joint) out[i] += mesh.skin[i].weights[k];
    return out;
}

std::vector<std::uint32_t> weight_heatmap_colors(const MeshData& mesh, std::uint32_t joint) {
    std::vector<std::uint32_t> out(mesh.vertices.size(), pack(0.02f, 0.02f, 0.08f));
    if (!mesh.skinned()) return out;
    for (std::size_t i = 0; i < mesh.skin.size(); ++i) {
        const SkinVertex& v = mesh.skin[i];
        if (joint == kDominantJoint) {
            // Golden-ratio hue per joint, brightness by the dominant weight (blends read darker).
            const float hue = std::fmod(static_cast<float>(v.joints[0]) * 0.61803398875f, 1.0f);
            const float h6 = hue * 6.0f;
            const float f = h6 - std::floor(h6);
            const float val = 0.35f + 0.65f * v.weights[0];
            float r = 0, g = 0, b = 0;
            switch (static_cast<int>(h6) % 6) {
            case 0: r = 1, g = f; break;
            case 1: r = 1 - f, g = 1; break;
            case 2: g = 1, b = f; break;
            case 3: g = 1 - f, b = 1; break;
            case 4: r = f, b = 1; break;
            default: r = 1, b = 1 - f; break;
            }
            out[i] = pack(r * val, g * val, b * val);
            continue;
        }
        float w = 0;
        for (int k = 0; k < 4; ++k)
            if (v.joints[k] == joint) w += v.weights[k];
        if (!(w > 0.0f)) continue;
        // Blue -> cyan -> green -> yellow -> red, like most DCC weight views.
        const float t = math::saturate(w);
        const float r = t < 0.5f ? 0.0f : 2.0f * t - 1.0f;
        const float g = t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
        const float b = t < 0.5f ? 1.0f - 2.0f * t : 0.0f;
        out[i] = pack(r, g, b);
    }
    return out;
}

// ---- deformation metrics ----------------------------------------------------------------------

float signed_volume(std::span<const Vertex3D> v, std::span<const std::uint32_t> idx) noexcept {
    double vol = 0;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (idx[t] >= v.size() || idx[t + 1] >= v.size() || idx[t + 2] >= v.size()) continue;
        const Vec3 a = v[idx[t]].position, b = v[idx[t + 1]].position, c = v[idx[t + 2]].position;
        vol += static_cast<double>(math::dot(a, math::cross(b, c)));
    }
    return static_cast<float>(vol / 6.0);
}

DeformationMetrics measure_deformation(std::span<const Vertex3D> rest, std::span<const Vertex3D> deformed,
                                       std::span<const std::uint32_t> idx) {
    DeformationMetrics m;
    if (rest.size() != deformed.size()) return m;
    m.rest_volume = signed_volume(rest, idx);
    m.deformed_volume = signed_volume(deformed, idx);
    m.volume_ratio = std::fabs(m.rest_volume) > 1e-12f ? m.deformed_volume / m.rest_volume : 1.0f;
    for (std::size_t i = 0; i < rest.size(); ++i)
        m.max_displacement = std::max(m.max_displacement, math::length(deformed[i].position - rest[i].position));
    bool first = true;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const std::uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= rest.size() || b >= rest.size() || c >= rest.size()) continue;
        const Vec3 n0 = math::cross(rest[b].position - rest[a].position, rest[c].position - rest[a].position);
        const float a0 = math::length(n0);
        if (a0 < 1e-14f) continue;
        const Vec3 n1 = math::cross(deformed[b].position - deformed[a].position, deformed[c].position - deformed[a].position);
        const float ratio = math::length(n1) / a0;
        if (first) m.min_area_ratio = m.max_area_ratio = ratio;
        m.min_area_ratio = std::min(m.min_area_ratio, ratio);
        m.max_area_ratio = std::max(m.max_area_ratio, ratio);
        first = false;
        if (ratio < 0.1f) ++m.collapsed_triangles;
        const Vec3 vn = deformed[a].normal + deformed[b].normal + deformed[c].normal;
        const Vec3 rn = rest[a].normal + rest[b].normal + rest[c].normal;
        // Only judge triangles whose rest face agreed with its vertex normals (well-formed input).
        if (math::dot(n0, rn) > 0.0f && math::dot(n1, vn) < 0.0f) ++m.flipped_triangles;
    }
    return m;
}

float local_volume_ratio(const MeshData& mesh, std::span<const Vertex3D> deformed, std::span<const std::uint32_t> joints,
                         float min_weight) {
    if (!mesh.skinned() || deformed.size() != mesh.vertices.size()) return 1.0f;
    std::vector<std::uint8_t> in(mesh.vertices.size(), 0);
    for (std::size_t i = 0; i < mesh.skin.size(); ++i) {
        float w = 0;
        for (int k = 0; k < 4; ++k)
            if (std::find(joints.begin(), joints.end(), mesh.skin[i].joints[k]) != joints.end()) w += mesh.skin[i].weights[k];
        in[i] = w >= min_weight ? 1 : 0;
    }
    // Volume of the region relative to its own centroid (open patches: the cone volume from the
    // centroid approximates the enclosed volume and is translation-invariant).
    std::vector<std::uint32_t> tri;
    Vec3 c0{}, c1{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < in.size(); ++i)
        if (in[i]) {
            c0 = c0 + mesh.vertices[i].position;
            c1 = c1 + deformed[i].position;
            ++n;
        }
    if (n == 0) return 1.0f;
    c0 = c0 / static_cast<float>(n);
    c1 = c1 / static_cast<float>(n);
    double v0 = 0, v1 = 0;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
        if (!in[a] || !in[b] || !in[c]) continue;
        v0 += math::dot(mesh.vertices[a].position - c0, math::cross(mesh.vertices[b].position - c0, mesh.vertices[c].position - c0));
        v1 += math::dot(deformed[a].position - c1, math::cross(deformed[b].position - c1, deformed[c].position - c1));
    }
    return std::fabs(v0) > 1e-18 ? static_cast<float>(v1 / v0) : 1.0f;
}

} // namespace victus::render
