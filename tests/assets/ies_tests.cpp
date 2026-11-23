// SPDX-License-Identifier: Apache-2.0
// IES photometric profiles (ADR-0235): LM-63 parsing, symmetry folding, the lat-long cookie bake
// and import as a texture asset.
#include "doctest.h"
#include "victus/assets/database.hpp"
#include "victus/assets/ies.hpp"
#include "victus/assets/importers.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"

#include <filesystem>
#include <string>

using namespace victus;
using namespace victus::assets;

namespace {
// A rotationally symmetric downlight: 1000 cd straight down falling to 0 at the horizon.
constexpr const char* kDownlight = "IESNA:LM-63-2002\r\n"
                                   "[TEST] downlight\r\n"
                                   "[MANUFAC] VICTUS\r\n"
                                   "TILT=NONE\r\n"
                                   "1 1200 2.0 4 1 1 2 0.1 0.1 0.05\r\n"
                                   "1.0 1.0 15\r\n"
                                   "0 30 60 90\r\n"
                                   "0\r\n"
                                   "500 400 100 0\r\n";

std::uint8_t grey(const render::Image& img, std::uint32_t x, std::uint32_t y) { return img.pixels[(std::size_t{y} * img.width + x) * 4]; }
} // namespace

TEST_CASE("ies: LM-63 downlight parses with the multiplier and samples between angles (ADR-0235)") {
    auto p = parse_ies(kDownlight);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.error().message));
    CHECK(p->lamps == 1);
    CHECK(p->lumens_per_lamp == doctest::Approx(1200.0f));
    CHECK(p->peak_candela == doctest::Approx(1000.0f)); // 500 x multiplier 2
    CHECK(p->sample(0.0f, 0.0f) == doctest::Approx(1000.0f));
    CHECK(p->sample(15.0f, 123.0f) == doctest::Approx(900.0f)); // rotational: any horizontal angle
    CHECK(p->sample(45.0f, -40.0f) == doctest::Approx(500.0f));
    CHECK(p->sample(90.0f, 0.0f) == doctest::Approx(0.0f));
    CHECK(p->sample(135.0f, 0.0f) == doctest::Approx(0.0f)); // above the measured range: dark

    // The cookie: bottom rows (down) bright, the upper half black, the same in every column.
    const render::Image img = ies_cookie_image(*p, 64);
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    CHECK(grey(img, 32, 63) > 250);
    CHECK(grey(img, 5, 63) == grey(img, 50, 63));
    CHECK(grey(img, 32, 10) == 0);
    CHECK(grey(img, 32, 31) == 0); // just above the horizon
    CHECK(grey(img, 32, 48) > 0);
    CHECK(grey(img, 32, 48) < grey(img, 32, 60));
}

TEST_CASE("ies: horizontal symmetry folds quadrant and bilateral tables, full tables wrap (ADR-0235)") {
    // Quadrant symmetric (0..90): 0 deg 100 cd, 45 deg 200 cd, 90 deg 300 cd at every vertical angle.
    const std::string quadrant = "IESNA91\nTILT=NONE\n1 -1 1 2 3 1 2 0 0 0\n1 1 10\n0 180\n0 45 90\n100 100\n200 200\n300 300\n";
    auto q = parse_ies(quadrant);
    REQUIRE(q.ok());
    CHECK(q->sample(10.0f, 90.0f) == doctest::Approx(300.0f));
    CHECK(q->sample(10.0f, 270.0f) == doctest::Approx(300.0f));
    CHECK(q->sample(10.0f, 135.0f) == doctest::Approx(200.0f));
    CHECK(q->sample(10.0f, 180.0f) == doctest::Approx(100.0f));
    CHECK(q->sample(10.0f, 22.5f) == doctest::Approx(150.0f));
    // Bilateral (0..180) with comma separators.
    auto b = parse_ies("TILT=NONE\n1,-1,1,2,2,1,2,0,0,0\n1,1,10\n0,180\n0,180\n10,10\n50,50\n");
    REQUIRE(b.ok());
    CHECK(b->sample(90.0f, 270.0f) == doctest::Approx(30.0f)); // 360 - 270 = 90: halfway
    // Full table without a closing 360 column wraps from 270 back to 0.
    auto f = parse_ies("TILT=NONE\n1 -1 1 2 4 1 2 0 0 0\n1 1 10\n0 180\n0 90 180 270\n10 10\n400 400\n10 10\n10 10\n");
    REQUIRE(f.ok());
    CHECK(f->sample(90.0f, 315.0f) == doctest::Approx(10.0f));
    CHECK(f->sample(90.0f, 45.0f) == doctest::Approx(205.0f));
    // The 90 degree (right) column of the cookie's horizon row is the brightest.
    const render::Image img = ies_cookie_image(*f, 64);
    const std::uint32_t horizon = 31, right = 48, forward = 32, left = 16;
    CHECK(grey(img, right, horizon) > grey(img, forward, horizon) + 100);
    CHECK(grey(img, right, horizon) > grey(img, left, horizon) + 100);
}

TEST_CASE("ies: TILT=INCLUDE is skipped and malformed files are rejected (ADR-0235)") {
    auto t = parse_ies("IESNA:LM-63-1995\nTILT=INCLUDE\n1\n3\n0 45 90\n1 0.9 0.8\n1 -1 1 1 1 1 2 0 0 0\n1 1 10\n0\n0\n42\n");
    REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
    CHECK(t->peak_candela == doctest::Approx(42.0f));
    CHECK_FALSE(parse_ies("no tilt line\n1 2 3\n").ok());
    CHECK_FALSE(parse_ies("TILT=lamp.tlt\n").ok());
    CHECK_FALSE(parse_ies("TILT=NONE\n1 -1 1 1 1 2 2 0 0 0\n1 1 10\n0\n0\n42\n").ok()); // Type B
    CHECK_FALSE(parse_ies("TILT=NONE\n1 -1 1 2 1 1 2 0 0 0\n1 1 10\n0 90\n0\n42\n").ok()); // truncated
    CHECK_FALSE(parse_ies("TILT=NONE\n1 -1 1 2 1 1 2 0 0 0\n1 1 10\n90 0\n0\n1 2\n").ok());  // descending
}

TEST_CASE("ies: .ies files import as sRGB 256 x 256 cookie textures (ADR-0235)") {
    CHECK(kind_for_extension(".ies") == AssetKind::Texture);
    CHECK(kind_for_extension(".IES") == AssetKind::Texture);
    const auto dir = std::filesystem::temp_directory_path() / ("victus_ies_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(dir);
    REQUIRE(fs::write_text_atomic(dir / "downlight.ies", kDownlight).ok());
    auto tex = import_texture(dir / "downlight.ies", TextureImportSettings{});
    REQUIRE_MESSAGE(tex.ok(), (tex.ok() ? "" : tex.error().message));
    CHECK(tex->srgb);
    REQUIRE(tex->mips.size() == 1);
    CHECK(tex->mips[0].width == 256);
    CHECK(tex->mips[0].height == 256);
    CHECK(grey(tex->mips[0], 128, 255) > 250);
    CHECK(grey(tex->mips[0], 128, 20) == 0);
    REQUIRE(fs::write_text_atomic(dir / "broken.ies", "TILT=NONE\n1 2\n").ok());
    CHECK_FALSE(import_texture(dir / "broken.ies", TextureImportSettings{}).ok());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
