// SPDX-License-Identifier: Apache-2.0
// Asset Store installer (ADR-0174): staging, licence gate, path safety, checksums, cancellation,
// texture-set processing (ORM packing, opacity, DirectX normals), .vmat generation, provenance,
// CREDITS.md and uninstall, all offline against canned downloads.
#include "doctest.h"
#include "fixtures.hpp"
#include "victus/assets/database.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/material.hpp"
#include "victus/core/zip.hpp"
#include "victus/library/install.hpp"

#include "stb_image.h"

#include <filesystem>

using namespace victus;
using namespace victus::library;
using library::test::FixtureHttp;
using library::test::png;
using library::test::TempProject;
namespace sfs = std::filesystem;

namespace {

// A provider whose plan is whatever the test sets.
class FakeProvider final : public Provider {
public:
    ProviderInfo info_{.id = "fake", .name = "Fake", .homepage = "https://example.com", .credit = "Fake credit"};
    InstallPlan next;
    const ProviderInfo& info() const override { return info_; }
    Result<SearchPage> search(HttpClient&, const SearchQuery&) override { return SearchPage{}; }
    Result<InstallPlan> plan(HttpClient&, const LibraryAsset&, std::string_view) override { return next; }
};

LibraryAsset fake_asset(Licence licence = licence_cc0()) {
    LibraryAsset a;
    a.provider = "fake";
    a.id = "Thing01";
    a.name = "Thing 01";
    a.type = AssetType::Model;
    a.author = "Someone";
    a.page_url = "https://example.com/thing";
    a.licence = std::move(licence);
    return a;
}

FileRequest file(std::string url, std::string rel) {
    FileRequest f;
    f.url = std::move(url);
    f.relative = std::move(rel);
    return f;
}

std::string read(const fs::Path& p) { return fs::read_text(p).value_or(std::string{}); }

bool staging_empty(const fs::Path& root) {
    std::error_code ec;
    const fs::Path s = root / ".victus" / "library_staging";
    return !sfs::exists(s, ec) || sfs::is_empty(s, ec);
}

} // namespace

TEST_CASE("install: a model lands in assets/library with licence records and credits") {
    TempProject proj("victus_lib_model");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    FixtureHttp http;
    http.responses["https://example.com/thing.glb"] = "glTF-not-really";
    FakeProvider p;
    p.next.files = {file("https://example.com/thing.glb", "thing.glb")};
    p.next.primary = "thing.glb";
    std::vector<float> fractions;
    InstallOptions o;
    o.progress = [&](const InstallProgress& pr) { fractions.push_back(pr.fraction); };
    auto r = install(p, http, fake_asset(licence_cc_by()), **db, o);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    CHECK(r->folder == "library/fake/thing01");
    CHECK(r->primary == "library/fake/thing01/thing.glb");
    CHECK_FALSE(r->primary_uuid.is_nil());
    const fs::Path dir = proj.root / "assets" / "library" / "fake" / "thing01";
    CHECK(read(dir / "thing.glb") == "glTF-not-really");
    CHECK(read(dir / "LICENSE.txt").find("Creative Commons Attribution 4.0") != std::string::npos);
    auto prov = json::parse(read(dir / "provenance.json"));
    REQUIRE(prov.ok());
    CHECK(prov.value()["asset"]["licence"]["id"].as_string() == "CC-BY-4.0");
    CHECK(prov.value()["source_urls"][0].as_string() == "https://example.com/thing.glb");
    const std::string credits = read(proj.root / "CREDITS.md");
    CHECK(credits.find("\"Thing 01\" by Someone") != std::string::npos);
    CHECK(credits.find("attribution required") != std::string::npos);
    CHECK(staging_empty(proj.root));
    REQUIRE_FALSE(fractions.empty());
    CHECK(std::is_sorted(fractions.begin(), fractions.end()));
    // The installed list reads the provenance back.
    const auto installed = installed_assets(proj.root);
    REQUIRE(installed.size() == 1);
    CHECK(installed[0].asset.key() == "fake:Thing01");
    // Installing again is a no-op that finds the same asset.
    const std::size_t requests = http.requested.size();
    auto again = install(p, http, fake_asset(licence_cc_by()), **db);
    REQUIRE(again.ok());
    CHECK(again->already_installed);
    CHECK(again->primary_uuid == r->primary_uuid);
    CHECK(http.requested.size() == requests);
    // Uninstall removes the folder and the credit.
    REQUIRE(uninstall(**db, r->folder).ok());
    CHECK_FALSE(sfs::exists(dir));
    CHECK_FALSE(sfs::exists(proj.root / "CREDITS.md"));
    CHECK_FALSE(uninstall(**db, "characters").ok()); // only Asset Store folders
}

TEST_CASE("install: licences that forbid commercial use or modification are refused before any download") {
    TempProject proj("victus_lib_licence");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    FixtureHttp http;
    FakeProvider p;
    p.next.files = {file("https://example.com/thing.glb", "thing.glb")};
    for (const char* label : {"CC Attribution-NonCommercial", "CC Attribution-NoDerivs"}) {
        auto r = install(p, http, fake_asset(licence_from_cc_label(label)), **db);
        CHECK_FALSE(r.ok());
    }
    CHECK(http.requested.empty());
    CHECK_FALSE(sfs::exists(proj.root / "assets" / "library"));
}

TEST_CASE("install: unsafe paths, plain HTTP and checksum mismatches leave nothing behind") {
    TempProject proj("victus_lib_safety");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    FixtureHttp http;
    http.responses["https://example.com/a.glb"] = "abc";
    FakeProvider p;
    for (const char* bad : {"../escape.glb", "/abs.glb", "a/../../b.glb", ""}) {
        p.next.files = {file("https://example.com/a.glb", bad)};
        CHECK_MESSAGE(!install(p, http, fake_asset(), **db).ok(), bad);
    }
    p.next.files = {file("http://example.com/a.glb", "a.glb")};
    CHECK_FALSE(install(p, http, fake_asset(), **db).ok());
    FileRequest f = file("https://example.com/a.glb", "a.glb");
    f.md5 = "00000000000000000000000000000000";
    p.next.files = {f};
    auto r = install(p, http, fake_asset(), **db);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("checksum") != std::string::npos);
    f.md5 = "900150983CD24FB0D6963F7D28E17F72"; // md5("abc"), case-insensitive
    p.next.files = {f};
    CHECK(install(p, http, fake_asset(), **db).ok());
    CHECK_FALSE(sfs::exists(proj.root / "escape.glb"));
    CHECK_FALSE(sfs::exists(proj.root / "assets" / "escape.glb"));
    CHECK(staging_empty(proj.root));
}

TEST_CASE("install: cancelled and failed downloads never appear in the project") {
    TempProject proj("victus_lib_cancel");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    FixtureHttp http;
    http.responses["https://example.com/a.glb"] = "abc";
    http.responses["https://example.com/b.glb"] = "def";
    FakeProvider p;
    p.next.files = {file("https://example.com/a.glb", "a.glb"), file("https://example.com/b.glb", "b.glb")};
    std::atomic<bool> cancel{false};
    InstallOptions o;
    o.cancel = &cancel;
    o.progress = [&](const InstallProgress& pr) {
        if (pr.stage.starts_with("Downloading 2/")) cancel = true; // cancel mid-way
    };
    auto r = install(p, http, fake_asset(), **db, o);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(sfs::exists(proj.root / "assets" / "library" / "fake"));
    CHECK(staging_empty(proj.root));
    http.fail_downloads = true;
    CHECK_FALSE(install(p, http, fake_asset(), **db).ok());
    CHECK_FALSE(sfs::exists(proj.root / "assets" / "library" / "fake"));
    // Size limit from the plan, before downloading.
    http.fail_downloads = false;
    p.next.files[0].bytes = 5ull << 30;
    const std::size_t before = http.requested.size();
    CHECK_FALSE(install(p, http, fake_asset(), **db).ok());
    CHECK(http.requested.size() == before);
}

TEST_CASE("install: texture sets become a .vmat with a packed ORM and linear data textures") {
    TempProject proj("victus_lib_material");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    // An ambientCG-style zip: separate maps, a DirectX normal, an opacity map and files VICTUS skips.
    zip::Writer w;
    REQUIRE(w.add("Grate01_1K-JPG_Color.png", png(4, 4, 3, 200), 0).ok());
    REQUIRE(w.add("Grate01_1K-JPG_NormalDX.png", png(4, 4, 3, 100), 0).ok());
    REQUIRE(w.add("Grate01_1K-JPG_Roughness.png", png(4, 4, 1, 64), 0).ok());
    REQUIRE(w.add("Grate01_1K-JPG_Metalness.png", png(4, 4, 1, 255), 0).ok());
    REQUIRE(w.add("Grate01_1K-JPG_Opacity.png", png(4, 4, 1, 10), 0).ok());
    REQUIRE(w.add("Grate01_1K-JPG.blend", std::string_view("blender"), 0).ok());
    REQUIRE(w.add("Grate01_1K-JPG.mtlx", std::string_view("<x/>"), 0).ok());
    const auto zipped = w.finish();
    FixtureHttp http;
    http.responses["https://ambientcg.com/get?file=Grate01_1K-JPG.zip"] = std::string(zipped.begin(), zipped.end());
    FakeProvider p;
    FileRequest z = file("https://ambientcg.com/get?file=Grate01_1K-JPG.zip", "");
    z.unzip = true;
    z.keep = {".jpg", ".png"};
    p.next.files = {z};
    p.next.make_material = true;
    p.next.material.base_color = "Grate01_1K-JPG_Color.png";
    p.next.material.normal = "Grate01_1K-JPG_NormalDX.png";
    p.next.material.normal_directx = true;
    p.next.material.roughness = "Grate01_1K-JPG_Roughness.png";
    p.next.material.metallic = "Grate01_1K-JPG_Metalness.png";
    p.next.material.occlusion = "Grate01_1K-JPG_AmbientOcclusion.png"; // not in the zip: skipped
    p.next.material.opacity = "Grate01_1K-JPG_Opacity.png";
    p.next.material.height = "Grate01_1K-JPG_Displacement.png"; // not in the zip
    LibraryAsset a = fake_asset();
    a.id = "Grate01";
    a.type = AssetType::Material;
    auto r = install(p, http, a, **db);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    CHECK(r->material == "library/fake/grate01/grate01.vmat");
    CHECK(r->primary == r->material);
    const fs::Path dir = proj.root / "assets" / "library" / "fake" / "grate01";
    CHECK_FALSE(sfs::exists(dir / "Grate01_1K-JPG.blend"));
    CHECK_FALSE(sfs::exists(dir / "Grate01_1K-JPG_Roughness.png")); // packed into the ORM
    CHECK_FALSE(sfs::exists(dir / "Grate01_1K-JPG_Opacity.png"));   // merged into the colour alpha
    REQUIRE(sfs::exists(dir / "grate01_orm.png"));
    // ORM channels: R = 255 (no AO map), G = roughness, B = metallic.
    int iw = 0, ih = 0, n = 0;
    stbi_uc* orm = stbi_load((dir / "grate01_orm.png").string().c_str(), &iw, &ih, &n, 3);
    REQUIRE(orm);
    CHECK(orm[0] == 255);
    CHECK(orm[1] == 64);
    CHECK(orm[2] == 255);
    stbi_image_free(orm);
    stbi_uc* nrm = stbi_load((dir / "Grate01_1K-JPG_NormalDX_gl.png").string().c_str(), &iw, &ih, &n, 3);
    REQUIRE(nrm);
    CHECK(nrm[1] == 155); // green flipped to the OpenGL convention
    stbi_image_free(nrm);
    stbi_uc* rgba = stbi_load((dir / "Grate01_1K-JPG_Color_rgba.png").string().c_str(), &iw, &ih, &n, 4);
    REQUIRE(rgba);
    CHECK(rgba[3] == 10);
    stbi_image_free(rgba);

    const auto* rec = (*db)->find_path(r->material);
    REQUIRE(rec);
    auto mat = (*db)->load_material(rec->uuid);
    REQUIRE(mat.ok());
    const auto* color = (*db)->find_path("library/fake/grate01/Grate01_1K-JPG_Color_rgba.png");
    const auto* ormrec = (*db)->find_path("library/fake/grate01/grate01_orm.png");
    const auto* nrec = (*db)->find_path("library/fake/grate01/Grate01_1K-JPG_NormalDX_gl.png");
    REQUIRE((color && ormrec && nrec));
    CHECK(mat->base_color_texture == color->uuid);
    CHECK(mat->metallic_roughness_texture == ormrec->uuid);
    CHECK(mat->normal_texture == nrec->uuid);
    CHECK(mat->metallic == doctest::Approx(1.0f));
    CHECK(mat->roughness == doctest::Approx(1.0f));
    CHECK(mat->occlusion_strength == doctest::Approx(0.0f)); // no AO map shipped
    CHECK(mat->alpha_cutoff == doctest::Approx(0.5f));
    CHECK(mat->double_sided);
    // Data textures are imported linear; colour stays sRGB with coverage-preserving mips.
    CHECK_FALSE(assets::TextureImportSettings::from_json(ormrec->settings).srgb);
    CHECK_FALSE(assets::TextureImportSettings::from_json(nrec->settings).srgb);
    const auto cs = assets::TextureImportSettings::from_json(color->settings);
    CHECK(cs.srgb);
    CHECK(cs.alpha_coverage_cutoff == doctest::Approx(0.5f));
    CHECK(cs.compression == "auto");
    // Provenance lists the final files, including the .vmat.
    auto prov = json::parse(read(dir / "provenance.json"));
    REQUIRE(prov.ok());
    bool has_vmat = false;
    for (const auto& f : prov.value()["files"].as_array()) has_vmat = has_vmat || f.as_string() == "grate01.vmat";
    CHECK(has_vmat);
    CHECK(prov.value()["primary"].as_string() == "grate01.vmat");
    // CC0 credits do not claim attribution is required.
    CHECK(read(proj.root / "CREDITS.md").find(" - **attribution required**") == std::string::npos);
}

TEST_CASE("install: a pre-packed ORM (Poly Haven 'arm') is used as-is") {
    TempProject proj("victus_lib_arm");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    FixtureHttp http;
    http.responses["https://dl.example.com/diff.png"] = png(2, 2, 3, 128);
    http.responses["https://dl.example.com/arm.png"] = png(2, 2, 3, 77);
    FakeProvider p;
    p.next.files = {file("https://dl.example.com/diff.png", "rock_diff.png"), file("https://dl.example.com/arm.png", "rock_arm.png")};
    p.next.make_material = true;
    p.next.material.base_color = "rock_diff.png";
    p.next.material.occlusion = p.next.material.roughness = p.next.material.metallic = "rock_arm.png";
    LibraryAsset a = fake_asset();
    a.id = "rock";
    auto r = install(p, http, a, **db);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    const fs::Path dir = proj.root / "assets" / "library" / "fake" / "rock";
    CHECK(sfs::exists(dir / "rock_arm.png"));
    CHECK_FALSE(sfs::exists(dir / "rock_orm.png"));
    auto mat = (*db)->load_material((*db)->find_path(r->material)->uuid);
    REQUIRE(mat.ok());
    CHECK(mat->metallic_roughness_texture == (*db)->find_path("library/fake/rock/rock_arm.png")->uuid);
    CHECK(mat->occlusion_strength == doctest::Approx(1.0f));
}

TEST_CASE("install: downloaded Kenney packs import one model format plus textures and sounds") {
    TempProject proj("victus_lib_pack");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    LibraryAsset a;
    a.provider = "kenney";
    a.id = "nature-kit";
    a.name = "Nature Kit";
    a.type = AssetType::Model;
    a.author = "Kenney";
    a.page_url = "https://kenney.nl/assets/nature-kit";
    a.licence = licence_cc0();
    auto r = import_archive(a, fs::Path(VICTUS_LIBRARY_DATA_DIR) / "kenney_pack.zip", **db);
    REQUIRE_MESSAGE(r.ok(), r.error().message);
    const fs::Path dir = proj.root / "assets" / "library" / "kenney" / "nature-kit";
    CHECK(sfs::exists(dir / "Models" / "GLB format" / "tree.glb"));
    CHECK_FALSE(sfs::exists(dir / "Models" / "FBX format" / "tree.fbx"));
    CHECK_FALSE(sfs::exists(dir / "Models" / "OBJ format" / "tree.obj"));
    CHECK(sfs::exists(dir / "Audio" / "rustle.ogg"));
    CHECK_FALSE(sfs::exists(dir / "__MACOSX"));
    CHECK(r->primary == "library/kenney/nature-kit/Models/GLB format/tree.glb");
    CHECK(read(dir / "LICENSE.txt").find("CC0") != std::string::npos);
    CHECK(read(proj.root / "CREDITS.md").find("Nature Kit") != std::string::npos);
}

TEST_CASE("install: stage() touches nothing under assets/ until commit()") {
    TempProject proj("victus_lib_stage");
    auto db = assets::AssetDatabase::open(proj.root);
    REQUIRE(db.ok());
    FixtureHttp http;
    http.responses["https://example.com/a.ogg"] = "OggS";
    FakeProvider p;
    p.next.files = {file("https://example.com/a.ogg", "a.ogg")};
    auto staged = stage(p, http, fake_asset(), proj.root);
    REQUIRE(staged.ok());
    CHECK_FALSE(sfs::exists(proj.root / "assets" / "library"));
    CHECK(sfs::exists(staged->staging / "a.ogg"));
    discard(staged.value());
    CHECK(staging_empty(proj.root));
    auto again = stage(p, http, fake_asset(), proj.root);
    REQUIRE(again.ok());
    auto r = commit(std::move(again.value()), **db);
    REQUIRE(r.ok());
    CHECK(sfs::exists(proj.root / "assets" / "library" / "fake" / "thing01" / "a.ogg"));
    // Replace keeps a single copy.
    InstallOptions o;
    o.replace = true;
    http.responses["https://example.com/a.ogg"] = "OggS2";
    REQUIRE(install(p, http, fake_asset(), **db, o).ok());
    CHECK(read(proj.root / "assets" / "library" / "fake" / "thing01" / "a.ogg") == "OggS2");
    CHECK(staging_empty(proj.root));
}
