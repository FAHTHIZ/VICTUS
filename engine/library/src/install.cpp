// SPDX-License-Identifier: Apache-2.0
#include "victus/library/install.hpp"

#include "victus/assets/importers.hpp"
#include "victus/assets/material.hpp"
#include "victus/core/log.hpp"
#include "victus/core/zip.hpp"

#include "provider_util.hpp"

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <set>

namespace victus::library {

namespace sfs = std::filesystem;
using detail::lower;

// ---- md5 (RFC 1321) ---------------------------------------------------------------------------

std::string md5_hex(std::span<const std::uint8_t> bytes) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr std::array<std::uint32_t, 64> r = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                                        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                                        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                                        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::uint64_t bit_len = static_cast<std::uint64_t>(bytes.size()) * 8u;
    const std::size_t total = ((bytes.size() + 8) / 64 + 1) * 64;
    std::array<std::uint8_t, 64> block{};
    for (std::size_t off = 0; off < total; off += 64) {
        for (std::size_t i = 0; i < 64; ++i) {
            const std::size_t at = off + i;
            if (at < bytes.size()) block[i] = bytes[at];
            else if (at == bytes.size()) block[i] = 0x80;
            else if (at >= total - 8) block[i] = static_cast<std::uint8_t>(bit_len >> (8 * (at - (total - 8))));
            else block[i] = 0;
        }
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = static_cast<std::uint32_t>(block[i * 4]) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (std::uint32_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0, g = 0;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            const std::uint32_t tmp = d;
            d = c;
            c = b;
            const std::uint32_t x = a + f + k[i] + m[g];
            b = b + ((x << r[i]) | (x >> (32 - r[i])));
            a = tmp;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    std::string out;
    for (const std::uint32_t v : h)
        for (int i = 0; i < 4; ++i) out += std::format("{:02x}", (v >> (8 * i)) & 0xffu);
    return out;
}

// ---- image helpers ----------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> inflate_raw(std::span<const std::uint8_t> compressed, std::size_t expected_size) {
    if (expected_size > (1ull << 31) - 1 || compressed.size() > (1ull << 31) - 1)
        return make_error(ErrorCode::LimitExceeded, "archive entry too large");
    std::vector<std::uint8_t> out(expected_size + 1); // +1 so a longer stream is detected
    const int n = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()),
                                                   reinterpret_cast<const char*>(compressed.data()),
                                                   static_cast<int>(compressed.size()));
    if (n < 0 || static_cast<std::size_t>(n) != expected_size)
        return make_error(ErrorCode::ParseError, "corrupt deflate data in archive");
    out.resize(expected_size);
    return out;
}

namespace {

struct Image {
    int w{0}, h{0};
    std::vector<std::uint8_t> px; // `channels` per pixel
};

Result<Image> load_image(const fs::Path& path, int channels) {
    auto bytes = fs::read_binary(path, 512ull << 20);
    if (!bytes) return bytes.error();
    Image img;
    int n = 0;
    stbi_uc* data = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &img.w, &img.h, &n, channels);
    if (!data) return make_error(ErrorCode::ParseError, std::format("cannot decode {}: {}", path.filename().string(), stbi_failure_reason()));
    img.px.assign(data, data + static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h) * static_cast<std::size_t>(channels));
    stbi_image_free(data);
    return img;
}

std::vector<std::uint8_t> encode_png(int w, int h, int channels, const std::uint8_t* px) {
    std::vector<std::uint8_t> out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
            const auto* b = static_cast<const std::uint8_t*>(data);
            v->insert(v->end(), b, b + size);
        },
        &out, w, h, channels, px, w * channels);
    return out;
}

} // namespace

Result<std::vector<std::uint8_t>> pack_orm(const fs::Path& occlusion, const fs::Path& roughness, const fs::Path& metallic,
                                           float default_roughness) {
    std::array<std::optional<Image>, 3> maps;
    const std::array<const fs::Path*, 3> paths = {&occlusion, &roughness, &metallic};
    int w = 0, h = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        if (paths[i]->empty()) continue;
        auto img = load_image(*paths[i], 1);
        if (!img) return img.error();
        if (w == 0) w = img->w, h = img->h;
        else if (img->w != w || img->h != h)
            return make_error(ErrorCode::InvalidArgument, "occlusion, roughness and metallic maps differ in size");
        maps[i] = std::move(img.value());
    }
    if (w == 0) return make_error(ErrorCode::InvalidArgument, "no maps to pack");
    const std::array<std::uint8_t, 3> fallback = {255, static_cast<std::uint8_t>(std::clamp(default_roughness, 0.0f, 1.0f) * 255.0f + 0.5f), 0};
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> rgb(count * 3);
    for (std::size_t p = 0; p < count; ++p)
        for (std::size_t c = 0; c < 3; ++c) rgb[p * 3 + c] = maps[c] ? maps[c]->px[p] : fallback[c];
    return encode_png(w, h, 3, rgb.data());
}

Result<std::vector<std::uint8_t>> flip_normal_green(const fs::Path& normal) {
    auto img = load_image(normal, 3);
    if (!img) return img.error();
    for (std::size_t p = 1; p < img->px.size(); p += 3) img->px[p] = static_cast<std::uint8_t>(255 - img->px[p]);
    return encode_png(img->w, img->h, 3, img->px.data());
}

Result<std::vector<std::uint8_t>> merge_opacity(const fs::Path& base_color, const fs::Path& opacity) {
    auto color = load_image(base_color, 4);
    if (!color) return color.error();
    auto alpha = load_image(opacity, 1);
    if (!alpha) return alpha.error();
    if (color->w != alpha->w || color->h != alpha->h)
        return make_error(ErrorCode::InvalidArgument, "colour and opacity maps differ in size");
    for (std::size_t p = 0; p < alpha->px.size(); ++p) color->px[p * 4 + 3] = alpha->px[p];
    return encode_png(color->w, color->h, 4, color->px.data());
}

// ---- records ----------------------------------------------------------------------------------

std::string install_folder(const LibraryAsset& asset) {
    return std::format("{}/{}/{}", kLibraryFolder, slugify(asset.provider), slugify(asset.id));
}

namespace {

std::string now_iso8601() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

bool cancelled(const InstallOptions& o) { return o.cancel && o.cancel->load(); }

void report(const InstallOptions& o, std::string stage, float fraction) {
    if (o.progress) o.progress({std::move(stage), std::clamp(fraction, 0.0f, 1.0f)});
}

std::string licence_text(const LibraryAsset& a) {
    std::string t;
    t += std::format("{}\n{}\n\n", a.name, std::string(a.name.size(), '='));
    t += std::format("Provider:  {}\n", a.provider);
    t += std::format("Source:    {}\n", a.page_url);
    if (!a.author.empty()) t += std::format("Author:    {}\n", a.author);
    t += std::format("Licence:   {}\n", a.licence.name);
    if (!a.licence.url.empty()) t += std::format("Licence text: {}\n", a.licence.url);
    t += std::format("Attribution required: {}\n", a.licence.attribution_required ? "yes" : "no");
    t += std::format("\nCredit line:\n  {}\n", a.attribution());
    if (!a.licence.notes.empty()) t += std::format("\nNotes:\n  {}\n", a.licence.notes);
    t += "\nInstalled by the VICTUS Studio Asset Store. provenance.json in this folder records the exact\n"
         "files and download URLs. Keep this file with the asset.\n";
    return t;
}

Status write_records(const StagedInstall& s) {
    json::Value p = json::Value::object();
    p.set("victus_library", kProvenanceVersion);
    p.set("asset", s.asset.to_json());
    p.set("variant", s.variant);
    p.set("installed_at", now_iso8601());
    json::Value files = json::Value::array();
    for (const auto& f : s.files) files.push(f);
    p.set("files", std::move(files));
    json::Value urls = json::Value::array();
    for (const auto& u : s.source_urls) urls.push(u);
    p.set("source_urls", std::move(urls));
    p.set("primary", s.primary);
    VICTUS_TRY_STATUS(fs::write_text_atomic(s.staging / std::string(kProvenanceFile), json::stringify(p, true)));
    return fs::write_text_atomic(s.staging / "LICENSE.txt", licence_text(s.asset));
}

bool has_suffix(std::string_view name, const std::vector<std::string>& keep) {
    if (keep.empty()) return true;
    const std::string l = lower(name);
    return std::any_of(keep.begin(), keep.end(), [&](const std::string& k) { return l.ends_with(k); });
}

// Extracts `entries` under `dest/prefix` and appends the written relative paths to `files`.
Status extract(const std::vector<zip::Entry>& entries, const fs::Path& dest, std::string_view prefix,
               const std::vector<std::string>& keep, std::vector<std::string>& files, const InstallOptions& o) {
    for (const auto& e : entries) {
        if (cancelled(o)) return make_error(ErrorCode::Cancelled, "install cancelled");
        if (e.name.starts_with("__MACOSX/") || e.name.find("/.") != std::string::npos || e.name.starts_with(".")) continue;
        if (!has_suffix(e.name, keep)) continue;
        const std::string joined = prefix.empty() ? e.name : std::string(prefix) + "/" + e.name;
        auto rel = fs::sanitize_relative(joined);
        if (!rel) return make_error(ErrorCode::InvalidArgument, "unsafe path in archive: " + e.name);
        auto path = fs::resolve_under(dest, rel.value());
        if (!path) return path.error();
        std::error_code ec;
        sfs::create_directories(path->parent_path(), ec);
        VICTUS_TRY_STATUS(fs::write_atomic(path.value(), e.data));
        files.push_back(rel.value());
    }
    return {};
}

Result<std::vector<zip::Entry>> read_archive(const fs::Path& archive, const InstallOptions& o) {
    auto bytes = fs::read_binary(archive, o.max_total_bytes);
    if (!bytes) return bytes.error();
    zip::ReadOptions ro;
    ro.inflate = inflate_raw;
    ro.max_total_bytes = o.max_total_bytes;
    ro.skip_directories = true;
    return zip::read(bytes.value(), ro);
}

fs::Path staging_root(const fs::Path& project_root) { return project_root / ".victus" / "library_staging"; }

Result<StagedInstall> begin(const LibraryAsset& asset, const fs::Path& project_root, const InstallOptions& o) {
    if (!asset.licence.installable())
        return detail::require_installable(asset).error();
    StagedInstall s;
    s.asset = asset;
    s.variant = o.variant;
    s.folder = install_folder(asset);
    const fs::Path final_dir = project_root / "assets" / s.folder;
    std::error_code ec;
    if (!o.replace && sfs::is_regular_file(final_dir / std::string(kProvenanceFile), ec)) {
        s.already_installed = true;
        auto text = fs::read_text(final_dir / std::string(kProvenanceFile));
        if (text)
            if (auto v = json::parse(text.value())) s.primary = v.value()["primary"].as_string_or("");
        return s;
    }
    s.staging = staging_root(project_root) / slugify(asset.key());
    sfs::remove_all(s.staging, ec);
    sfs::create_directories(s.staging, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", s.staging.string(), ec.message()));
    return s;
}

std::string path_with_extension(std::string_view rel, std::string_view suffix_ext) {
    const std::size_t dot = rel.rfind('.');
    return std::string(rel.substr(0, dot == std::string_view::npos ? rel.size() : dot)) + std::string(suffix_ext);
}

void forget(StagedInstall& s, const std::string& rel) {
    std::error_code ec;
    sfs::remove(s.staging / rel, ec);
    s.files.erase(std::remove(s.files.begin(), s.files.end(), rel), s.files.end());
}

// Normalises a staged texture set: drops maps that were not shipped, converts DirectX normals,
// merges opacity into the colour alpha and packs AO/roughness/metallic into one ORM texture.
Status finish_material(StagedInstall& s) {
    MaterialRecipe& m = s.material;
    std::error_code ec;
    for (std::string* slot : {&m.base_color, &m.normal, &m.roughness, &m.metallic, &m.occlusion, &m.emissive, &m.height, &m.opacity})
        if (!slot->empty() && !sfs::is_regular_file(s.staging / *slot, ec)) slot->clear();
    if (m.base_color.empty()) return make_error(ErrorCode::NotFound, "the download has no colour map");
    if (!m.normal.empty() && m.normal_directx) {
        auto png = flip_normal_green(s.staging / m.normal);
        if (!png) return png.error();
        const std::string out = path_with_extension(m.normal, "_gl.png");
        VICTUS_TRY_STATUS(fs::write_atomic(s.staging / out, png.value()));
        forget(s, m.normal);
        s.files.push_back(out);
        m.normal = out;
        m.normal_directx = false;
    }
    if (!m.opacity.empty()) {
        auto png = merge_opacity(s.staging / m.base_color, s.staging / m.opacity);
        if (!png) return png.error();
        const std::string out = path_with_extension(m.base_color, "_rgba.png");
        VICTUS_TRY_STATUS(fs::write_atomic(s.staging / out, png.value()));
        forget(s, m.base_color);
        forget(s, m.opacity);
        s.files.push_back(out);
        m.base_color = out;
        m.opacity.clear();
        s.alpha_masked = true;
    }
    const bool packed = !m.occlusion.empty() && m.occlusion == m.roughness && m.roughness == m.metallic;
    if (!packed && (!m.occlusion.empty() || !m.roughness.empty() || !m.metallic.empty())) {
        const auto at = [&](const std::string& r) { return r.empty() ? fs::Path{} : s.staging / r; };
        auto png = pack_orm(at(m.occlusion), at(m.roughness), at(m.metallic));
        if (!png) return png.error();
        const std::string out = slugify(s.asset.id) + "_orm.png";
        VICTUS_TRY_STATUS(fs::write_atomic(s.staging / out, png.value()));
        for (const std::string* r : {&m.occlusion, &m.roughness, &m.metallic})
            if (!r->empty()) forget(s, *r);
        s.files.push_back(out);
        // Remember which channels carry real data: AO strength stays 0 without an AO map.
        const bool had_ao = !m.occlusion.empty();
        m.occlusion = had_ao ? out : std::string{};
        m.roughness = m.metallic = out;
    }
    std::sort(s.files.begin(), s.files.end());
    s.files.erase(std::unique(s.files.begin(), s.files.end()), s.files.end());
    return {};
}

} // namespace

Result<StagedInstall> stage(Provider& provider, HttpClient& http, const LibraryAsset& asset, const fs::Path& project_root,
                            const InstallOptions& o) {
    if (provider.info().id != asset.provider)
        return make_error(ErrorCode::InvalidArgument, "asset belongs to provider '" + asset.provider + "'");
    auto begun = begin(asset, project_root, o);
    if (!begun || begun->already_installed) return begun;
    StagedInstall s = std::move(begun.value());
    const auto fail = [&](Error e) -> Result<StagedInstall> {
        discard(s);
        return e;
    };
    report(o, "Resolving download", 0.0f);
    auto planned = provider.plan(http, asset, o.variant);
    if (!planned) return fail(planned.error());
    InstallPlan plan = std::move(planned.value());
    if (plan.files.empty()) return fail(make_error(ErrorCode::NotFound, "nothing to download"));
    std::uint64_t expected = 0;
    for (const auto& f : plan.files) expected += f.bytes;
    if (expected > o.max_total_bytes)
        return fail(make_error(ErrorCode::LimitExceeded, std::format("download is {}, above the {} limit", format_bytes(expected),
                                                                      format_bytes(o.max_total_bytes))));
    std::uint64_t done = 0;
    for (std::size_t i = 0; i < plan.files.size(); ++i) {
        const FileRequest& f = plan.files[i];
        if (cancelled(o)) return fail(make_error(ErrorCode::Cancelled, "install cancelled"));
        if (!is_safe_https_url(f.url)) return fail(make_error(ErrorCode::InvalidArgument, "refusing non-HTTPS download: " + f.url));
        std::string rel;
        if (!f.unzip || !f.relative.empty()) {
            auto r = fs::sanitize_relative(f.relative);
            if (!r || r->empty()) return fail(make_error(ErrorCode::InvalidArgument, "unsafe destination path: " + f.relative));
            rel = r.value();
        }
        const fs::Path dest = f.unzip ? s.staging / std::format(".download{}.zip", i) : s.staging / rel;
        std::error_code ec;
        sfs::create_directories(dest.parent_path(), ec);
        HttpRequest req;
        req.url = f.url;
        req.headers = f.headers;
        req.expected_bytes = f.bytes;
        req.max_bytes = std::min<std::uint64_t>(o.max_total_bytes - std::min(done, o.max_total_bytes),
                                                f.bytes ? f.bytes + f.bytes / 8 + (1u << 20) : o.max_total_bytes);
        req.timeout_seconds = 1800;
        const std::string label = f.unzip ? asset.name : sfs::path(rel).filename().string();
        const std::uint64_t base = done;
        const auto progress = [&](std::uint64_t got, std::uint64_t total) {
            const double denom = static_cast<double>(expected ? expected : (total ? total : 1));
            report(o, std::format("Downloading {}/{}: {}", i + 1, plan.files.size(), label),
                   0.9f * static_cast<float>(static_cast<double>(base + got) / denom));
        };
        report(o, std::format("Downloading {}/{}: {}", i + 1, plan.files.size(), label),
               expected ? 0.9f * static_cast<float>(static_cast<double>(done) / static_cast<double>(expected)) : 0.0f);
        if (Status st = http.download(req, dest, progress, o.cancel); !st) return fail(st.error());
        const auto size = sfs::file_size(dest, ec);
        done += ec ? 0 : size;
        if (!f.md5.empty()) {
            auto bytes = fs::read_binary(dest, o.max_total_bytes);
            if (!bytes) return fail(bytes.error());
            if (md5_hex(bytes.value()) != lower(f.md5))
                return fail(make_error(ErrorCode::ParseError, "checksum mismatch for " + f.url + " (corrupt or changed download)"));
        }
        s.source_urls.push_back(f.url);
        if (f.unzip) {
            report(o, "Extracting " + asset.name, 0.9f);
            auto entries = read_archive(dest, o);
            if (!entries) return fail(entries.error());
            if (Status st = extract(entries.value(), s.staging, rel, f.keep, s.files, o); !st) return fail(st.error());
            sfs::remove(dest, ec);
        } else {
            s.files.push_back(rel);
        }
    }
    s.make_material = plan.make_material;
    s.material = plan.material;
    s.primary = plan.primary;
    if (s.make_material) {
        report(o, "Preparing material textures", 0.93f);
        if (Status st = finish_material(s); !st) return fail(st.error());
        s.primary = s.material.base_color; // replaced by the .vmat in commit()
    }
    if (std::find(s.files.begin(), s.files.end(), s.primary) == s.files.end() && !s.files.empty()) s.primary = s.files.front();
    if (Status st = write_records(s); !st) return fail(st.error());
    report(o, "Ready to add", 0.95f);
    return s;
}

namespace {

// Chooses one model format per pack (Kenney ships GLB, glTF, FBX and OBJ copies of each model).
std::vector<std::string> pack_keep_list(const std::vector<zip::Entry>& entries) {
    std::set<std::string> present;
    for (const auto& e : entries) present.insert(sfs::path(lower(e.name)).extension().string());
    std::vector<std::string> keep = {".png", ".jpg", ".jpeg", ".ogg", ".wav", ".mp3", ".flac", ".ttf", ".otf"};
    if (present.contains(".glb")) keep.push_back(".glb");
    else if (present.contains(".gltf")) keep.insert(keep.end(), {".gltf", ".bin"});
    else if (present.contains(".fbx")) keep.push_back(".fbx");
    else if (present.contains(".obj")) keep.insert(keep.end(), {".obj", ".mtl"});
    return keep;
}

} // namespace

Result<StagedInstall> stage_archive(const LibraryAsset& asset, const fs::Path& archive, const fs::Path& project_root,
                                    const InstallOptions& o) {
    auto begun = begin(asset, project_root, o);
    if (!begun || begun->already_installed) return begun;
    StagedInstall s = std::move(begun.value());
    report(o, "Reading " + archive.filename().string(), 0.1f);
    auto entries = read_archive(archive, o);
    if (!entries) {
        discard(s);
        return entries.error();
    }
    if (Status st = extract(entries.value(), s.staging, "", pack_keep_list(entries.value()), s.files, o); !st) {
        discard(s);
        return st.error();
    }
    if (s.files.empty()) {
        discard(s);
        return make_error(ErrorCode::NotFound, "the archive has no models, textures or sounds VICTUS can import");
    }
    std::sort(s.files.begin(), s.files.end());
    for (const auto& f : s.files) {
        const std::string ext = sfs::path(lower(f)).extension().string();
        if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj") {
            s.primary = f;
            break;
        }
    }
    if (s.primary.empty()) s.primary = s.files.front();
    s.source_urls.push_back(asset.page_url);
    if (Status st = write_records(s); !st) {
        discard(s);
        return st.error();
    }
    return s;
}

void discard(const StagedInstall& staged) {
    if (staged.staging.empty()) return;
    std::error_code ec;
    sfs::remove_all(staged.staging, ec);
}

Result<InstallResult> commit(StagedInstall s, assets::AssetDatabase& db) {
    InstallResult result;
    result.folder = s.folder;
    if (s.already_installed) {
        result.already_installed = true;
        result.primary = s.primary.empty() ? s.folder : s.folder + "/" + s.primary;
        if (const auto* r = db.find_path(result.primary)) result.primary_uuid = r->uuid;
        return result;
    }
    const fs::Path final_dir = db.assets_dir() / s.folder;
    std::error_code ec;
    sfs::create_directories(final_dir.parent_path(), ec);
    // Replace atomically enough: move the old copy aside, move the new one in, drop the old.
    const fs::Path backup = staging_root(db.project_root()) / (slugify(s.asset.key()) + ".previous");
    sfs::remove_all(backup, ec);
    const bool had_old = sfs::exists(final_dir, ec);
    if (had_old) {
        sfs::rename(final_dir, backup, ec);
        if (ec) {
            discard(s);
            return make_error(ErrorCode::IoError, std::format("cannot replace {}: {}", final_dir.string(), ec.message()));
        }
    }
    sfs::rename(s.staging, final_dir, ec);
    if (ec) {
        if (had_old) sfs::rename(backup, final_dir, ec);
        discard(s);
        return make_error(ErrorCode::IoError, std::format("cannot move the asset into {}: {}", final_dir.string(), ec.message()));
    }
    sfs::remove_all(backup, ec);
    (void)db.refresh();

    const auto uuid_of = [&](const std::string& rel) -> Uuid {
        if (rel.empty()) return {};
        const auto* r = db.find_path(s.folder + "/" + rel);
        return r ? r->uuid : Uuid{};
    };
    // Data textures are linear; every library texture gets GPU block compression variants.
    const auto configure = [&](const std::string& rel, bool srgb, float coverage) {
        if (rel.empty()) return;
        const auto* r = db.find_path(s.folder + "/" + rel);
        if (!r || r->importer != "texture") return;
        assets::TextureImportSettings t = assets::TextureImportSettings::from_json(r->settings);
        t.srgb = srgb;
        t.compression = "auto";
        t.alpha_coverage_cutoff = coverage;
        if (Status st = db.set_settings(r->uuid, t.to_json()); !st)
            VICTUS_LOG_WARN("library", "cannot set import settings of {}: {}", rel, st.error().message);
    };
    for (const auto& f : s.files) result.files.push_back(s.folder + "/" + f);
    result.primary = s.folder + "/" + s.primary;
    if (s.make_material) {
        const MaterialRecipe& m = s.material;
        configure(m.base_color, true, s.alpha_masked ? 0.5f : 0.0f);
        configure(m.normal, false, 0.0f);
        configure(m.roughness, false, 0.0f);
        configure(m.emissive, true, 0.0f);
        configure(m.height, false, 0.0f);
        assets::MaterialAsset mat;
        mat.base_color_texture = uuid_of(m.base_color);
        mat.normal_texture = uuid_of(m.normal);
        mat.metallic_roughness_texture = uuid_of(m.roughness);
        if (!mat.metallic_roughness_texture.is_nil()) {
            mat.metallic = 1.0f; // factors multiply the ORM channels
            mat.roughness = 1.0f;
            mat.occlusion_strength = m.occlusion.empty() ? 0.0f : 1.0f;
        }
        if (!m.emissive.empty()) {
            mat.emissive_texture = uuid_of(m.emissive);
            mat.emissive = {1, 1, 1, 1};
        }
        if (s.alpha_masked) {
            mat.alpha_cutoff = 0.5f;
            mat.double_sided = true;
        }
        const std::string vmat = slugify(s.asset.id) + std::string(assets::kMaterialExtension);
        if (Status st = assets::save_material_file(final_dir / vmat, mat); !st) return st.error();
        (void)db.refresh();
        result.material = s.folder + "/" + vmat;
        result.files.push_back(result.material);
        result.primary = result.material;
        // Keep provenance in step with the folder contents.
        s.files.push_back(vmat);
        s.primary = vmat;
        s.staging = final_dir;
        (void)write_records(s);
    }
    if (const auto* r = db.find_path(result.primary)) result.primary_uuid = r->uuid;
    if (Status st = write_credits(db.project_root()); !st)
        VICTUS_LOG_WARN("library", "cannot update CREDITS.md: {}", st.error().message);
    return result;
}

Result<InstallResult> install(Provider& provider, HttpClient& http, const LibraryAsset& asset, assets::AssetDatabase& db,
                              const InstallOptions& options) {
    auto staged = stage(provider, http, asset, db.project_root(), options);
    if (!staged) return staged.error();
    return commit(std::move(staged.value()), db);
}

Result<InstallResult> import_archive(const LibraryAsset& asset, const fs::Path& archive, assets::AssetDatabase& db,
                                     const InstallOptions& options) {
    auto staged = stage_archive(asset, archive, db.project_root(), options);
    if (!staged) return staged.error();
    return commit(std::move(staged.value()), db);
}

// ---- installed assets -------------------------------------------------------------------------

std::vector<InstalledAsset> installed_assets(const fs::Path& project_root, std::vector<std::string>* errors) {
    std::vector<InstalledAsset> out;
    const fs::Path lib = project_root / "assets" / std::string(kLibraryFolder);
    std::error_code ec;
    if (!sfs::is_directory(lib, ec)) return out;
    for (const auto& provider : sfs::directory_iterator(lib, ec)) {
        if (!provider.is_directory(ec)) continue;
        for (const auto& dir : sfs::directory_iterator(provider.path(), ec)) {
            const fs::Path record = dir.path() / std::string(kProvenanceFile);
            if (!sfs::is_regular_file(record, ec)) continue;
            const std::string folder = std::format("{}/{}/{}", kLibraryFolder, provider.path().filename().string(),
                                                   dir.path().filename().string());
            auto text = fs::read_text(record, 16ull << 20);
            auto parsed = text ? json::parse(text.value()) : Result<json::Value>(text.error());
            if (!parsed) {
                if (errors) errors->push_back(folder + ": " + parsed.error().message);
                continue;
            }
            const json::Value& v = parsed.value();
            auto asset = LibraryAsset::from_json(v["asset"]);
            if (!asset) {
                if (errors) errors->push_back(folder + ": " + asset.error().message);
                continue;
            }
            InstalledAsset ia;
            ia.asset = std::move(asset.value());
            ia.folder = folder;
            ia.variant = v["variant"].as_string_or("");
            ia.installed_at = v["installed_at"].as_string_or("");
            for (const auto& f : v["files"].as_array())
                if (f.is_string()) ia.files.push_back(f.as_string());
            out.push_back(std::move(ia));
        }
    }
    std::sort(out.begin(), out.end(), [](const InstalledAsset& a, const InstalledAsset& b) { return a.folder < b.folder; });
    return out;
}

Status write_credits(const fs::Path& project_root) {
    const auto installed = installed_assets(project_root);
    const fs::Path path = project_root / "CREDITS.md";
    std::error_code ec;
    if (installed.empty()) {
        // Only remove a file this function wrote.
        auto text = fs::read_text(path);
        if (text && text->starts_with("# Asset credits")) sfs::remove(path, ec);
        return {};
    }
    std::map<std::string, std::vector<const InstalledAsset*>> by_provider;
    for (const auto& ia : installed) by_provider[ia.asset.provider].push_back(&ia);
    std::string md = "# Asset credits\n\n"
                     "Third-party assets installed from the VICTUS Studio Asset Store. Generated from the\n"
                     "`provenance.json` records under `assets/library/`; exports ship this file next to the game.\n"
                     "Entries marked **attribution required** must stay credited wherever the game is distributed.\n";
    for (const auto& [provider, list] : by_provider) {
        md += std::format("\n## {}\n\n", provider);
        for (const InstalledAsset* ia : list) {
            md += std::format("- {}", ia->asset.attribution());
            if (ia->asset.licence.attribution_required) md += " - **attribution required**";
            md += std::format(" (`assets/{}`)\n", ia->folder);
        }
    }
    return fs::write_text_atomic(path, md);
}

Status uninstall(assets::AssetDatabase& db, std::string_view folder) {
    auto rel = fs::sanitize_relative(folder);
    if (!rel || !rel->starts_with(std::string(kLibraryFolder) + "/"))
        return make_error(ErrorCode::InvalidArgument, "not an Asset Store folder: " + std::string(folder));
    const fs::Path dir = db.assets_dir() / rel.value();
    std::error_code ec;
    if (!sfs::is_regular_file(dir / std::string(kProvenanceFile), ec))
        return make_error(ErrorCode::NotFound, "no installed asset at " + rel.value());
    sfs::remove_all(dir, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot remove {}: {}", dir.string(), ec.message()));
    (void)db.refresh();
    return write_credits(db.project_root());
}

} // namespace victus::library
