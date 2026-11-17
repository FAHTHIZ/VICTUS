// SPDX-License-Identifier: Apache-2.0
// Terrain data: the .vterrain format, sampling, generation and auto painting.
#include "victus/terrain/terrain.hpp"

#include "victus/core/fs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace victus::terrain {

namespace {

constexpr char kMagic[4] = {'V', 'T', 'R', 'N'};
constexpr std::size_t kHeaderBytes = 16;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint16_t get_u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t get_u32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}
std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

Error bad(std::string_view what) { return make_error(ErrorCode::ParseError, std::format("terrain: {}", what)); }

// Integer lattice hash -> [0, 1).
float lattice(std::uint64_t seed, std::int64_t x, std::int64_t z) {
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(z) * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<float>(h >> 40) / static_cast<float>(1u << 24);
}

float value_noise(std::uint64_t seed, float x, float z) {
    const float fx = std::floor(x), fz = std::floor(z);
    const auto ix = static_cast<std::int64_t>(fx), iz = static_cast<std::int64_t>(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * tx * (tx * (tx * 6 - 15) + 10); // quintic fade
    tz = tz * tz * tz * (tz * (tz * 6 - 15) + 10);
    const float a = lattice(seed, ix, iz), b = lattice(seed, ix + 1, iz);
    const float c = lattice(seed, ix, iz + 1), d = lattice(seed, ix + 1, iz + 1);
    return std::lerp(std::lerp(a, b, tx), std::lerp(c, d, tx), tz);
}

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

} // namespace

bool valid_resolution(std::uint32_t r) noexcept {
    if (r < kMinResolution || r > kMaxResolution) return false;
    const std::uint32_t q = r - 1;
    return (q & (q - 1)) == 0;
}

float Heightfield::height(std::int32_t x, std::int32_t z) const noexcept {
    if (resolution == 0) return 0.0f;
    const auto last = static_cast<std::int32_t>(resolution) - 1;
    x = std::clamp(x, 0, last);
    z = std::clamp(z, 0, last);
    return heights[index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z))];
}

float Heightfield::sample(float u, float v) const noexcept {
    if (resolution == 0) return 0.0f;
    const auto last = static_cast<float>(resolution - 1);
    const float x = std::clamp(u, 0.0f, 1.0f) * last, z = std::clamp(v, 0.0f, 1.0f) * last;
    const auto x0 = static_cast<std::int32_t>(std::floor(x)), z0 = static_cast<std::int32_t>(std::floor(z));
    const float tx = x - static_cast<float>(x0), tz = z - static_cast<float>(z0);
    return std::lerp(std::lerp(height(x0, z0), height(x0 + 1, z0), tx), std::lerp(height(x0, z0 + 1), height(x0 + 1, z0 + 1), tx), tz);
}

math::Vec4 Heightfield::weights(float u, float v) const noexcept {
    if (resolution == 0) return {1, 0, 0, 0};
    const auto last = static_cast<float>(resolution - 1);
    const float x = std::clamp(u, 0.0f, 1.0f) * last, z = std::clamp(v, 0.0f, 1.0f) * last;
    const auto lim = static_cast<std::int32_t>(resolution) - 1;
    const auto x0 = std::min(static_cast<std::int32_t>(std::floor(x)), lim), z0 = std::min(static_cast<std::int32_t>(std::floor(z)), lim);
    const auto x1 = std::min(x0 + 1, lim), z1 = std::min(z0 + 1, lim);
    const float tx = x - static_cast<float>(x0), tz = z - static_cast<float>(z0);
    auto w = [&](std::int32_t sx, std::int32_t sz) {
        return unpack_weights(splat[index(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sz))]);
    };
    const math::Vec4 a = w(x0, z0), b = w(x1, z0), c = w(x0, z1), d = w(x1, z1);
    math::Vec4 r{};
    for (int i = 0; i < 4; ++i) r[i] = std::lerp(std::lerp(a[i], b[i], tx), std::lerp(c[i], d[i], tx), tz);
    const float sum = r.x + r.y + r.z + r.w;
    return sum > 1e-6f ? r * (1.0f / sum) : math::Vec4{1, 0, 0, 0};
}

void Heightfield::touch(std::int32_t x0, std::int32_t z0, std::int32_t x1, std::int32_t z1) noexcept {
    ++revision;
    const std::uint32_t cps = chunks_per_side();
    if (cps == 0) return;
    if (chunk_revisions.size() != static_cast<std::size_t>(cps) * cps) chunk_revisions.assign(static_cast<std::size_t>(cps) * cps, 0);
    if (x1 < x0) std::swap(x0, x1);
    if (z1 < z0) std::swap(z0, z1);
    // Normals read one sample further; border samples belong to both neighbouring chunks.
    const auto q = static_cast<std::int32_t>(kChunkQuads), last = static_cast<std::int32_t>(cps) - 1;
    const auto chunk_lo = [&](std::int32_t s) { return std::clamp((s - 2) / q, 0, last); };
    const auto chunk_hi = [&](std::int32_t s) { return std::clamp((s + 1) / q, 0, last); };
    const std::int32_t cx0 = chunk_lo(std::max(x0, 0)), cx1 = chunk_hi(x1), cz0 = chunk_lo(std::max(z0, 0)), cz1 = chunk_hi(z1);
    if (x1 < 0 || z1 < 0) return;
    for (std::int32_t cz = cz0; cz <= cz1; ++cz)
        for (std::int32_t cx = cx0; cx <= cx1; ++cx)
            chunk_revisions[static_cast<std::size_t>(cz) * cps + static_cast<std::size_t>(cx)] = revision;
}

void Heightfield::touch_all() noexcept {
    if (resolution == 0) return;
    touch(0, 0, static_cast<std::int32_t>(resolution) - 1, static_cast<std::int32_t>(resolution) - 1);
}

std::uint32_t pack_weights(math::Vec4 w) noexcept {
    for (int i = 0; i < 4; ++i) w[i] = std::isfinite(w[i]) ? std::max(0.0f, w[i]) : 0.0f;
    const float sum = w.x + w.y + w.z + w.w;
    if (sum <= 1e-6f) return 255u;
    std::uint32_t out = 0;
    int bytes[4];
    int total = 0;
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<int>(std::lround(w[i] / sum * 255.0f));
        total += bytes[i];
    }
    // Keep the sum at exactly 255 so weights round-trip: fix the largest channel.
    int largest = 0;
    for (int i = 1; i < 4; ++i)
        if (bytes[i] > bytes[largest]) largest = i;
    bytes[largest] = std::clamp(bytes[largest] + (255 - total), 0, 255);
    for (int i = 0; i < 4; ++i) out |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    return out;
}

math::Vec4 unpack_weights(std::uint32_t p) noexcept {
    math::Vec4 w{static_cast<float>(p & 0xFF), static_cast<float>((p >> 8) & 0xFF), static_cast<float>((p >> 16) & 0xFF),
                 static_cast<float>(p >> 24)};
    const float sum = w.x + w.y + w.z + w.w;
    return sum > 0 ? w * (1.0f / sum) : math::Vec4{1, 0, 0, 0};
}

Result<Heightfield> make_flat(std::uint32_t resolution, float height) {
    if (!valid_resolution(resolution))
        return make_error(ErrorCode::InvalidArgument,
                          std::format("terrain: resolution {} must be 2^n + 1 between {} and {}", resolution, kMinResolution, kMaxResolution));
    Heightfield hf;
    hf.resolution = resolution;
    const std::size_t n = static_cast<std::size_t>(resolution) * resolution;
    hf.heights.assign(n, std::clamp(std::isfinite(height) ? height : 0.0f, 0.0f, 1.0f));
    hf.splat.assign(n, 255u);
    hf.touch_all();
    return hf;
}

std::vector<std::uint8_t> serialize(const Heightfield& hf) {
    std::vector<std::uint8_t> out;
    const std::size_t n = hf.heights.size();
    out.reserve(kHeaderBytes + n * 6 + 8);
    out.insert(out.end(), kMagic, kMagic + 4);
    put_u32(out, kTerrainVersion);
    put_u32(out, hf.resolution);
    put_u32(out, 0);
    for (const float h : hf.heights) {
        const float c = std::isfinite(h) ? std::clamp(h, 0.0f, 1.0f) : 0.0f;
        put_u16(out, static_cast<std::uint16_t>(std::lround(c * 65535.0f)));
    }
    for (const std::uint32_t s : hf.splat) put_u32(out, s);
    put_u64(out, fnv1a64(std::span<const std::uint8_t>(out)));
    return out;
}

Result<Heightfield> deserialize(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes + 8) return bad("file too small");
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return bad("not a .vterrain file");
    const std::uint32_t version = get_u32(bytes.data() + 4);
    if (version != kTerrainVersion) return bad(std::format("unsupported version {} (expected {})", version, kTerrainVersion));
    const std::uint32_t res = get_u32(bytes.data() + 8);
    if (!valid_resolution(res)) return bad(std::format("invalid resolution {}", res));
    const std::size_t n = static_cast<std::size_t>(res) * res;
    const std::size_t expected = kHeaderBytes + n * 6 + 8;
    if (bytes.size() != expected) return bad(std::format("size {} does not match resolution {} ({} bytes expected)", bytes.size(), res, expected));
    const std::uint64_t stored = get_u64(bytes.data() + expected - 8);
    if (stored != fnv1a64(bytes.subspan(0, expected - 8))) return bad("checksum mismatch (file is corrupt)");
    Heightfield hf;
    hf.resolution = res;
    hf.heights.resize(n);
    hf.splat.resize(n);
    const std::uint8_t* p = bytes.data() + kHeaderBytes;
    for (std::size_t i = 0; i < n; ++i, p += 2) hf.heights[i] = static_cast<float>(get_u16(p)) / 65535.0f;
    for (std::size_t i = 0; i < n; ++i, p += 4) hf.splat[i] = get_u32(p);
    hf.touch_all();
    return hf;
}

Result<Heightfield> load_file(const std::filesystem::path& path) {
    auto bytes = fs::read_binary(path, 8ull * kMaxResolution * kMaxResolution);
    if (!bytes) return std::move(bytes).error();
    return deserialize(*bytes);
}

Status save_file(const std::filesystem::path& path, const Heightfield& hf) {
    if (!valid_resolution(hf.resolution) || hf.heights.size() != static_cast<std::size_t>(hf.resolution) * hf.resolution ||
        hf.splat.size() != hf.heights.size())
        return make_error(ErrorCode::InvalidArgument, "terrain: heightfield is not valid");
    const std::vector<std::uint8_t> bytes = serialize(hf);
    return fs::write_atomic(path, bytes);
}

void generate(Heightfield& hf, const GenerateSettings& s) {
    if (hf.resolution == 0) return;
    const std::uint32_t octaves = std::clamp(s.octaves, 1u, 12u);
    float norm = 0, amp = 1;
    for (std::uint32_t o = 0; o < octaves; ++o, amp *= s.persistence) norm += amp;
    const float inv = norm > 0 ? 1.0f / norm : 1.0f;
    const auto last = static_cast<float>(hf.resolution - 1);
    const float ridge = std::clamp(s.ridge, 0.0f, 1.0f);
    for (std::uint32_t z = 0; z < hf.resolution; ++z)
        for (std::uint32_t x = 0; x < hf.resolution; ++x) {
            const float u = static_cast<float>(x) / last, v = static_cast<float>(z) / last;
            float sum = 0, a = 1, f = std::max(0.01f, s.frequency);
            for (std::uint32_t o = 0; o < octaves; ++o) {
                float n = value_noise(s.seed + o * 1013ull, u * f, v * f);
                const float r = 1.0f - std::abs(n * 2.0f - 1.0f);
                n = std::lerp(n, r * r, ridge);
                sum += n * a;
                a *= s.persistence;
                f *= 2.0f;
            }
            hf.heights[hf.index(x, z)] = std::clamp(s.base + sum * inv * s.amplitude, 0.0f, 1.0f);
        }
    hf.touch_all();
}

void auto_paint(Heightfield& hf, math::Vec3 size, const AutoPaintSettings& s) {
    if (hf.resolution == 0) return;
    const float cos_start = std::cos(math::radians(s.slope_start)), cos_end = std::cos(math::radians(s.slope_end));
    for (std::uint32_t z = 0; z < hf.resolution; ++z)
        for (std::uint32_t x = 0; x < hf.resolution; ++x) {
            const float h = hf.heights[hf.index(x, z)];
            const float ny = sample_normal(hf, size, static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)).y;
            const float rock = 1.0f - smoothstep(cos_end, cos_start, ny);
            const float sand = 1.0f - smoothstep(s.low - s.blend, s.low + s.blend, h);
            const float snow = smoothstep(s.high - s.blend, s.high + s.blend, h);
            math::Vec4 w{1, 0, 0, 0};
            w = w * (1 - sand) + math::Vec4{0, 0, 1, 0} * sand;
            w = w * (1 - snow) + math::Vec4{0, 0, 0, 1} * snow;
            w = w * (1 - rock) + math::Vec4{0, 1, 0, 0} * rock;
            hf.splat[hf.index(x, z)] = pack_weights(w);
        }
    hf.touch_all();
}

math::Vec3 sample_normal(const Heightfield& hf, math::Vec3 size, std::int32_t x, std::int32_t z) noexcept {
    if (hf.resolution < 2) return {0, 1, 0};
    const auto last = static_cast<float>(hf.resolution - 1);
    const float dx = size.x / last, dz = size.z / last;
    const float hl = hf.height(x - 1, z) * size.y, hr = hf.height(x + 1, z) * size.y;
    const float hd = hf.height(x, z - 1) * size.y, hu = hf.height(x, z + 1) * size.y;
    const auto lim = static_cast<std::int32_t>(hf.resolution) - 1;
    const float sx = (x > 0 && x < lim) ? 2.0f * dx : dx, sz = (z > 0 && z < lim) ? 2.0f * dz : dz;
    const math::Vec3 n{-(hr - hl) / sx, 1.0f, -(hu - hd) / sz};
    return math::normalize(n);
}

std::optional<float> local_height(const Heightfield& hf, math::Vec3 size, float x, float z) noexcept {
    if (hf.resolution < 2 || size.x <= 0 || size.z <= 0) return std::nullopt;
    const float u = x / size.x + 0.5f, v = z / size.z + 0.5f;
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) return std::nullopt;
    // Match the rendered triangles (split along the x = z diagonal of every quad), not bilinear.
    const auto last = static_cast<float>(hf.resolution - 1);
    const float gx = u * last, gz = v * last;
    const auto lim = static_cast<std::int32_t>(hf.resolution) - 2;
    const auto x0 = std::min(static_cast<std::int32_t>(gx), lim), z0 = std::min(static_cast<std::int32_t>(gz), lim);
    const float tx = gx - static_cast<float>(x0), tz = gz - static_cast<float>(z0);
    const float h00 = hf.height(x0, z0), h10 = hf.height(x0 + 1, z0), h01 = hf.height(x0, z0 + 1), h11 = hf.height(x0 + 1, z0 + 1);
    const float h = tx >= tz ? h00 + (h10 - h00) * tx + (h11 - h10) * tz : h00 + (h01 - h00) * tz + (h11 - h01) * tx;
    return h * size.y;
}

} // namespace victus::terrain
