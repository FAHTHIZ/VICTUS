// SPDX-License-Identifier: Apache-2.0
// GPU block compression in the cooker (Slice 49): BC1/BC3/BC7 and ASTC 4x4/6x6 encode and decode
// within quality bounds, cooked files carry the variants, and import settings select them.
#include "doctest.h"
#include "victus/assets/database.hpp"
#include "victus/assets/export_profile.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/texture_compression.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

using namespace victus;
using namespace victus::assets;

namespace {

// Smooth colour gradients with a soft circle: typical albedo content.
render::Image test_image(std::uint32_t w, std::uint32_t h, bool alpha = false, bool circle = true) {
    render::Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(std::size_t{w} * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float u = float(x) / float(w), v = float(y) / float(h);
            const float d = std::hypot(u - 0.5f, v - 0.5f);
            auto* p = img.pixels.data() + (std::size_t{y} * w + x) * 4;
            p[0] = std::uint8_t(std::lround(255.0f * u));
            p[1] = std::uint8_t(std::lround(255.0f * v));
            p[2] = circle ? std::uint8_t(d < 0.3f ? 220 : 40) : std::uint8_t(std::lround(128.0f + 100.0f * (u - v)));
            p[3] = alpha ? std::uint8_t(std::lround(255.0f * (1.0f - v))) : 255;
        }
    return img;
}

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("texture compression: setting strings") {
    CHECK(parse_compression_setting("none")->empty());
    CHECK(parse_compression_setting("")->empty());
    CHECK(*parse_compression_setting("auto") == std::vector{TextureCodec::BC7, TextureCodec::ASTC4x4});
    CHECK(*parse_compression_setting("small") == std::vector{TextureCodec::BC1, TextureCodec::ASTC6x6});
    CHECK(*parse_compression_setting("astc6x6+bc7+astc6x6") == std::vector{TextureCodec::ASTC6x6, TextureCodec::BC7});
    CHECK_FALSE(parse_compression_setting("etc2").ok());
    CHECK_FALSE(parse_compression_setting("bc7+").ok());
    CHECK(codec_format(TextureCodec::BC7, true) == rhi::Format::BC7_RGBA_SRGB);
    CHECK(codec_format(TextureCodec::ASTC6x6, false) == rhi::Format::ASTC_6x6_UNORM);
    CHECK(rhi::mip_size_bytes(rhi::Format::BC1_RGBA_SRGB, 13, 7) == 4 * 2 * 8);
    CHECK(rhi::mip_size_bytes(rhi::Format::ASTC_6x6_SRGB, 13, 7) == 3 * 2 * 16);
    CHECK(rhi::mip_size_bytes(rhi::Format::RGBA8_UNORM, 13, 7) == 13 * 7 * 4);
    CHECK(rhi::mip_size_bytes(rhi::Format::BC7_RGBA_UNORM, 1, 1) == 16);
}

TEST_CASE("texture compression: every codec round trips within its quality bound") {
    const render::Image img = test_image(64, 64);
    struct Case {
        TextureCodec codec;
        double min_psnr;
        double bits_per_texel;
    };
    for (const Case c : {Case{TextureCodec::BC1, 34.0, 4.0}, Case{TextureCodec::BC3, 34.0, 8.0}, Case{TextureCodec::BC7, 40.0, 8.0},
                         Case{TextureCodec::ASTC4x4, 40.0, 8.0}, Case{TextureCodec::ASTC6x6, 34.0, 128.0 / 36.0}}) {
        for (const bool srgb : {true, false}) {
            CAPTURE(to_string(c.codec));
            CAPTURE(srgb);
            auto enc = encode_texture(img, c.codec, srgb);
            REQUIRE(enc.ok());
            const double expected_bytes = std::ceil(64.0 / (c.codec == TextureCodec::ASTC6x6 ? 6 : 4));
            CHECK(enc->size() == std::size_t(expected_bytes * expected_bytes * (c.codec == TextureCodec::BC1 ? 8 : 16)));
            auto dec = decode_texture(*enc, 64, 64, c.codec, srgb);
            REQUIRE(dec.ok());
            const double psnr = image_psnr(img, *dec);
            MESSAGE(std::string(assets::to_string(c.codec)) << (srgb ? " srgb" : " linear") << " PSNR " << psnr << " dB at " << c.bits_per_texel << " bpp");
            CHECK(psnr >= c.min_psnr);
        }
    }
    // Wrong block payload sizes are rejected rather than read out of bounds.
    CHECK_FALSE(decode_texture(std::vector<std::uint8_t>(15), 4, 4, TextureCodec::BC7, true).ok());
    CHECK_FALSE(encode_texture(render::Image{}, TextureCodec::BC7, true).ok());
}

TEST_CASE("texture compression: alpha, odd sizes and full mip chains") {
    const render::Image rgba = test_image(32, 32, true);
    // BC1 has no alpha channel worth using: it is promoted to BC3 for translucent sources.
    auto bc = compress_mips(std::span(&rgba, 1), TextureCodec::BC1, true);
    REQUIRE(bc.ok());
    CHECK(bc->codec == TextureCodec::BC3);
    auto dec = decode_texture(bc->levels[0], 32, 32, TextureCodec::BC3, true);
    REQUIRE(dec.ok());
    CHECK(image_psnr(rgba, *dec, true) >= 34.0);
    for (const auto codec : {TextureCodec::BC7, TextureCodec::ASTC4x4}) {
        auto e = encode_texture(rgba, codec, true);
        REQUIRE(e.ok());
        CHECK(image_psnr(rgba, *decode_texture(*e, 32, 32, codec, true), true) >= 36.0);
    }
    const render::Image opaque = test_image(16, 16);
    CHECK(compress_mips(std::span(&opaque, 1), TextureCodec::BC1, true)->codec == TextureCodec::BC1);

    // A smooth 52x28 texture down to 1x1 (13x7, 6x3, 3x1 on the way).
    const CookedTexture t = cook_texture(test_image(52, 28, false, false), "odd", true, true, 4096);
    REQUIRE(t.mips.size() == 6);
    for (const auto codec : {TextureCodec::BC7, TextureCodec::ASTC6x6}) {
        auto mips = compress_mips(t.mips, codec, true);
        REQUIRE(mips.ok());
        REQUIRE(mips->levels.size() == t.mips.size());
        for (std::size_t m = 0; m < t.mips.size(); ++m) {
            CHECK(mips->levels[m].size() == rhi::mip_size_bytes(codec_format(codec, true), t.mips[m].width, t.mips[m].height));
            auto d = decode_texture(mips->levels[m], t.mips[m].width, t.mips[m].height, codec, true);
            REQUIRE(d.ok());
            CAPTURE(std::string(assets::to_string(codec)));
            CAPTURE(m);
            MESSAGE(std::string(assets::to_string(codec)) << " mip " << m << " PSNR " << image_psnr(t.mips[m], *d));
            // Tiny levels are almost all gradient per texel (a colour plane, not a line, per block),
            // so only a sanity bound applies there; real-size levels keep full quality.
            const bool tiny = std::min(t.mips[m].width, t.mips[m].height) < 14;
            CHECK(image_psnr(t.mips[m], *d) >= (tiny ? 16.0 : 32.0));
        }
    }
    // Partial edge blocks encode exactly like the image padded by replicating its border.
    const render::Image odd = test_image(13, 7);
    render::Image padded;
    padded.width = 16;
    padded.height = 8;
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const auto* p = odd.pixels.data() + (std::size_t{std::min(y, 6u)} * 13 + std::min(x, 12u)) * 4;
            padded.pixels.insert(padded.pixels.end(), p, p + 4);
        }
    CHECK(*encode_texture(odd, TextureCodec::BC7, true) == *encode_texture(padded, TextureCodec::BC7, true));
}

TEST_CASE("texture compression: cooked files carry variants and reject truncation") {
    CookedTexture t = cook_texture(test_image(32, 16), "albedo", true, true, 4096);
    REQUIRE(compress_cooked_texture(t, "auto").ok());
    REQUIRE(t.compressed.size() == 2);
    CHECK(t.compressed[0].codec == TextureCodec::BC7);
    CHECK(t.compressed[1].codec == TextureCodec::ASTC4x4);
    CHECK_FALSE(compress_cooked_texture(t, "dxt9").ok());
    REQUIRE(compress_cooked_texture(t, "small").ok());
    REQUIRE(t.compressed.size() == 2);

    CookedHeader header;
    header.kind = AssetKind::Texture;
    const auto bytes = serialize(header, t);
    auto back = deserialize_texture(bytes);
    REQUIRE(back.ok());
    REQUIRE(back->compressed.size() == 2);
    CHECK(back->compressed[0].codec == TextureCodec::BC1);
    CHECK(back->compressed[1].codec == TextureCodec::ASTC6x6);
    CHECK(back->compressed[1].levels == t.compressed[1].levels);
    for (std::size_t cut : {std::size_t{1}, std::size_t{17}, std::size_t{200}}) {
        const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(deserialize_texture(truncated).ok());
    }
    // An unknown codec byte is corruption, not a crash. The codec byte of the last variant sits
    // right after the variant count and every level of the first variant.
    std::size_t first = 0;
    for (const auto& l : t.compressed[0].levels) first += 8 + l.size();
    std::size_t last_variant = bytes.size();
    for (const auto& l : t.compressed[1].levels) last_variant -= 8 + l.size();
    auto bad = bytes;
    bad[last_variant - 1] = 99;
    CHECK_FALSE(deserialize_texture(bad).ok());
    (void)first;

    // HDR textures keep their preview uncompressed.
    CookedTexture hdr = t;
    hdr.hdr = true;
    REQUIRE(compress_cooked_texture(hdr, "auto").ok());
    CHECK(hdr.compressed.empty());
}

TEST_CASE("texture compression: import settings drive the cooker and the cache key") {
    TempDir project("victus_texcomp");
    const fs::Path tex = project.path / "assets" / "albedo.png";
    std::filesystem::create_directories(tex.parent_path());
    auto png = render::encode_png(test_image(64, 64));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(tex, *png).ok());

    TextureImportSettings s;
    s.compression = "bc7+astc4x4";
    auto t = import_texture(tex, s);
    REQUIRE(t.ok());
    REQUIRE(t->compressed.size() == 2);
    CHECK(t->compressed[0].levels.size() == t->mips.size());
    CHECK(TextureImportSettings::from_json(s.to_json()).compression == "bc7+astc4x4");
    CHECK(TextureImportSettings::from_json(TextureImportSettings{}.to_json()).compression == "none");
    s.compression = "nope";
    CHECK_FALSE(import_texture(tex, s).ok());
    ModelImportSettings ms;
    ms.texture_compression = "auto";
    CHECK(ModelImportSettings::from_json(ms.to_json()).texture_compression == "auto");

    auto opened = AssetDatabase::open(project.path);
    REQUIRE(opened.ok());
    auto& db = **opened;
    const AssetRecord* rec = db.find_path("albedo.png");
    REQUIRE(rec);
    const Uuid id = rec->uuid;
    REQUIRE(db.import_all().failed == 0);
    CHECK(db.load_texture(id)->compressed.empty());
    json::Value settings = rec->settings;
    settings.set("compression", "astc6x6");
    REQUIRE(db.set_settings(id, settings).ok());
    const ImportReport r = db.import_all();
    CHECK(r.imported == 1);
    auto cooked = db.load_texture(id);
    REQUIRE(cooked.ok());
    REQUIRE(cooked->compressed.size() == 1);
    CHECK(cooked->compressed[0].codec == TextureCodec::ASTC6x6);
}

TEST_CASE("texture export profiles: android keeps ASTC, caps size and strips large RGBA8 levels") {
    CHECK(texture_export_profile("all")->target == "all");
    CHECK(texture_export_profile("")->target == "all");
    CHECK(texture_export_profile("desktop")->drop_mobile_codecs);
    CHECK_FALSE(texture_export_profile("ps9").ok());
    const auto android = texture_export_profile("android");
    REQUIRE(android.ok());

    // 256x128 with a tiny cap so the size limit is exercised on a small image.
    CookedTexture t = cook_texture(test_image(256, 128), "albedo", true, true, 4096);
    REQUIRE(compress_cooked_texture(t, "bc7+astc4x4").ok());
    const CookedTexture original = t;
    TextureExportProfile p = *android;
    p.max_size = 128;
    TextureProfileStats stats;
    apply_texture_profile(t, p, &stats);
    CHECK(stats.textures == 1);
    CHECK(stats.levels_dropped == 1);
    CHECK(stats.variants_dropped == 1);
    REQUIRE(t.compressed.size() == 1);
    CHECK(t.compressed[0].codec == TextureCodec::ASTC4x4);
    REQUIRE(t.mips.size() == original.mips.size() - 1);
    CHECK(t.mips[0].width == 128);
    CHECK(t.compressed[0].levels.size() == t.mips.size());
    CHECK(t.compressed[0].levels[0] == original.compressed[1].levels[1]);
    CHECK_FALSE(t.has_rgba8());
    for (const auto& m : t.mips) CHECK(m.pixels.empty() == (std::max(m.width, m.height) > 64));
    CHECK(stats.rgba8_stripped == 1); // only the 128x64 level is above 64 px

    // The stripped file round trips and is far smaller; a stripped level decodes back on the CPU.
    CookedHeader header;
    header.kind = AssetKind::Texture;
    const auto full = serialize(header, original);
    const auto bytes = serialize(header, t);
    CHECK(bytes.size() * 4 < full.size());
    auto back = deserialize_texture(bytes);
    REQUIRE(back.ok());
    CHECK(back->mips[0].width == 128);
    CHECK(back->mips[0].pixels.empty());
    auto level0 = rgba8_level(*back, 0);
    REQUIRE(level0.ok());
    CHECK(level0->width == 128);
    CHECK(level0->pixels.size() == 128u * 64 * 4);
    CHECK(image_psnr(*level0, original.mips[1]) >= 32.0);
    CHECK(rgba8_level(*back, 1)->pixels == original.mips[2].pixels); // kept level is verbatim
    CHECK_FALSE(rgba8_level(*back, 99).ok());

    // The byte-level entry point gives the same result and leaves "all" untouched.
    auto via_bytes = apply_texture_profile(std::span(full), p);
    REQUIRE(via_bytes.ok());
    CHECK(*via_bytes == bytes);
    CHECK(*apply_texture_profile(std::span(full), *texture_export_profile("all")) == full);

    // A level without RGBA8 pixels and no variant to recover it is corrupt.
    CookedTexture broken = *back;
    broken.compressed.clear();
    CHECK_FALSE(deserialize_texture(serialize(header, broken)).ok());

    // Textures without an ASTC variant keep their RGBA8 levels; HDR textures are untouched.
    CookedTexture plain = cook_texture(test_image(64, 64), "plain", true, true, 4096);
    REQUIRE(compress_cooked_texture(plain, "bc7").ok());
    apply_texture_profile(plain, *android);
    CHECK(plain.compressed.empty());
    CHECK(plain.has_rgba8());
    CookedTexture hdr = original;
    hdr.hdr = true;
    apply_texture_profile(hdr, *android);
    CHECK(hdr.compressed.size() == 2);
    CHECK(hdr.has_rgba8());
}

namespace {
// Sets an environment variable for the scope of one test and restores the previous value.
struct ScopedEnv {
    std::string name;
    std::optional<std::string> old;
    ScopedEnv(const char* n, const std::string& value) : name(n) {
        if (const char* v = std::getenv(n)) old = v;
        set(value.c_str());
    }
    ~ScopedEnv() { set(old ? old->c_str() : nullptr); }
    void set(const char* v) const {
#if defined(_WIN32)
        _putenv_s(name.c_str(), v ? v : "");
#else
        if (v) setenv(name.c_str(), v, 1);
        else unsetenv(name.c_str());
#endif
    }
};
} // namespace

TEST_CASE("shared cooked cache: identical sources cook once across projects (ADR-0180)") {
    TempDir cache("victus_ddc");
    TempDir a("victus_ddc_a"), b("victus_ddc_b");
    ScopedEnv dir("VICTUS_SHARED_CACHE", cache.path.string());
    CHECK(shared_cooked_cache_dir() == cache.path);
    auto png = render::encode_png(test_image(512, 512));
    REQUIRE(png.ok());
    for (const TempDir* p : {&a, &b}) {
        std::filesystem::create_directories(p->path / "assets");
        REQUIRE(fs::write_atomic(p->path / "assets" / "albedo.png", *png).ok());
    }
    auto count_entries = [&] {
        std::size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(cache.path))
            if (e.path().extension() == ".vcooked") ++n;
        return n;
    };

    auto da = AssetDatabase::open(a.path);
    REQUIRE(da.ok());
    CHECK((*da)->import_all().imported == 1);
    CHECK((*da)->shared_cache_hits() == 0);
    CHECK(count_entries() == 1);

    // Another project with the same bytes and settings links the entry instead of cooking.
    auto dbb = AssetDatabase::open(b.path);
    REQUIRE(dbb.ok());
    auto& db = **dbb;
    CHECK(db.import_all().imported == 1);
    CHECK(db.shared_cache_hits() == 1);
    const Uuid ida = (*da)->find_path("albedo.png")->uuid, idb = db.find_path("albedo.png")->uuid;
    CHECK(ida != idb);
    CHECK(*fs::read_binary((*da)->cooked_path(ida)) == *fs::read_binary(db.cooked_path(idb)));
    CHECK(*db.is_up_to_date(idb));
    auto tex = db.load_texture(idb);
    REQUIRE(tex.ok());
    CHECK(tex->mips.front().width == 512);

    // Different settings are a different entry; the hit count stays.
    json::Value settings = db.find_path("albedo.png")->settings;
    settings.set("compression", "bc7");
    REQUIRE(db.set_settings(idb, settings).ok());
    CHECK(db.import_all().imported == 1);
    CHECK(db.shared_cache_hits() == 1);
    CHECK(count_entries() == 2);
    REQUIRE(db.load_texture(idb)->compressed.size() == 1);
    // The project's copy survives the shared entry going away (hard link or copy).
    std::filesystem::remove_all(cache.path);
    std::filesystem::create_directories(cache.path);
    CHECK(db.load_texture(idb).ok());

    // A zero size limit trims everything, including the entry just stored.
    {
        ScopedEnv limit("VICTUS_SHARED_CACHE_MB", "0");
        settings.set("compression", "bc1");
        REQUIRE(db.set_settings(idb, settings).ok());
        CHECK(db.import_all().imported == 1);
        CHECK(count_entries() == 0);
        CHECK(db.load_texture(idb).ok());
    }
    {
        ScopedEnv off("VICTUS_SHARED_CACHE", "off");
        CHECK(shared_cooked_cache_dir().empty());
    }
}

TEST_CASE("model import settings: rgba8_max_size keeps only small RGBA8 levels when compressed") {
    ModelImportSettings ms;
    CHECK(ModelImportSettings::from_json(ms.to_json()).rgba8_max_size == 0);
    CHECK(ms.to_json().find("rgba8_max_size") == nullptr);
    ms.rgba8_max_size = 256;
    CHECK(ModelImportSettings::from_json(ms.to_json()).rgba8_max_size == 256);

    CookedTexture t = cook_texture(test_image(1024, 1024), "albedo", true, true, 4096);
    CookedTexture plain = t;
    strip_rgba8_levels(plain, 256); // no compressed variant: nothing is stripped
    CHECK(plain.has_rgba8());
    REQUIRE(compress_cooked_texture(t, "bc7").ok());
    strip_rgba8_levels(t, 256);
    CHECK_FALSE(t.has_rgba8());
    for (const auto& m : t.mips) CHECK(m.pixels.empty() == (std::max(m.width, m.height) > 256));
    auto level0 = rgba8_level(t, 0); // decoded from BC7 on demand
    REQUIRE(level0.ok());
    CHECK(level0->width == 1024);
    CHECK(level0->pixels.size() == std::size_t{1024} * 1024 * 4);
}

TEST_CASE("texture compression: model decode keeps only the variant the device would upload") {
    CookedTexture t = cook_texture(test_image(32, 16), "albedo", true, true, 4096);
    REQUIRE(compress_cooked_texture(t, "auto").ok()); // BC7 then ASTC 4x4
    CookedModel model;
    ModelMaterial mat;
    mat.base_color_texture = 0;
    model.materials.push_back(mat);
    model.textures.push_back(t);
    model.meshes.push_back({"quad", render::make_plane({1, 1}, 1), 0});
    ModelNode node;
    node.name = "root";
    node.meshes = {0};
    model.nodes.push_back(node);
    CookedHeader h;
    const auto bytes = serialize(h, model);

    const auto only_astc = [](TextureCodec c, bool) { return c == TextureCodec::ASTC4x4; };
    auto phone = deserialize_model(bytes, only_astc);
    REQUIRE(phone.ok());
    REQUIRE(phone->textures[0].compressed.size() == 1);
    CHECK(phone->textures[0].compressed[0].codec == TextureCodec::ASTC4x4);
    CHECK(phone->textures[0].compressed[0].levels == t.compressed[1].levels);
    CHECK(phone->textures[0].mips.size() == t.mips.size());

    auto desktop = deserialize_model(bytes, [](TextureCodec, bool) { return true; }); // first accepted wins
    REQUIRE(desktop.ok());
    REQUIRE(desktop->textures[0].compressed.size() == 1);
    CHECK(desktop->textures[0].compressed[0].codec == TextureCodec::BC7);

    auto none = deserialize_model(bytes, [](TextureCodec, bool) { return false; }); // CPU decode needs them all
    REQUIRE(none.ok());
    CHECK(none->textures[0].compressed.size() == 2);
    CHECK(deserialize_model(bytes)->textures[0].compressed.size() == 2);

    auto one = deserialize_model_texture(bytes, 0, only_astc);
    REQUIRE(one.ok());
    REQUIRE(one->compressed.size() == 1);
    CHECK(one->compressed[0].codec == TextureCodec::ASTC4x4);
    for (std::size_t n = 0; n < bytes.size(); n += 53) CHECK_FALSE(deserialize_model(std::span(bytes.data(), n), only_astc).ok());
}
