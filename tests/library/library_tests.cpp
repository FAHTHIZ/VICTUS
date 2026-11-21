// SPDX-License-Identifier: Apache-2.0
// Asset Store catalogue (ADR-0174): licences, helpers and every provider's parsing of its API or
// page format, against canned responses (no network).
#include "doctest.h"
#include "fixtures.hpp"
#include "victus/core/zip.hpp"
#include "victus/library/install.hpp"
#include "victus/library/library.hpp"

#include <algorithm>

using namespace victus;
using namespace victus::library;
using library::test::FixtureHttp;

namespace {
std::span<const std::uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }
std::unique_ptr<Provider> provider(std::string_view id) {
    for (auto& p : make_all_providers())
        if (p->info().id == id) return std::move(p);
    return nullptr;
}
} // namespace

TEST_CASE("library: licences gate installation on permissions, not names") {
    CHECK(licence_cc0().installable());
    CHECK_FALSE(licence_cc0().attribution_required);
    CHECK(licence_public_domain_us_gov().installable());
    const Licence by = licence_from_cc_label("CC Attribution");
    CHECK(by.installable());
    CHECK(by.attribution_required);
    CHECK(by.id == "CC-BY-4.0");
    CHECK(licence_from_cc_label("CC Attribution-ShareAlike").share_alike);
    CHECK(licence_from_cc_label("cc0").id == "CC0-1.0");
    CHECK(licence_from_cc_label("CC0 Public Domain").installable());
    for (const char* blocked : {"CC Attribution-NonCommercial", "CC Attribution-NoDerivs", "by-nc", "by-nc-nd",
                                "CC Attribution-NonCommercial-ShareAlike", "Editorial", "Standard"})
        CHECK_MESSAGE(!licence_from_cc_label(blocked).installable(), blocked);
    // Records missing permission flags are not trusted.
    const Licence parsed = Licence::from_json(json::parse(R"({"id":"x","name":"x"})").value());
    CHECK_FALSE(parsed.installable());
    const Licence rt = Licence::from_json(by.to_json());
    CHECK(rt.installable());
    CHECK(rt.attribution_required);
}

TEST_CASE("library: helpers") {
    CHECK(url_encode("red brick & wall") == "red%20brick%20%26%20wall");
    CHECK(html_unescape("Tom &amp; Jerry&#39;s &quot;x&quot;") == "Tom & Jerry's \"x\"");
    CHECK(slugify("City Kit (Roads)") == "city-kit-roads");
    CHECK(slugify("../../etc") == "etc");
    CHECK_FALSE(slugify("!!!").empty());
    CHECK(format_bytes(1536) == "1.5 KB");
    CHECK(is_safe_https_url("https://dl.polyhaven.org/file/x.jpg"));
    CHECK_FALSE(is_safe_https_url("http://example.com/x"));
    CHECK_FALSE(is_safe_https_url("https://user:pw@example.com/x"));
    CHECK_FALSE(is_safe_https_url("https://exa mple.com/x"));
    CHECK_FALSE(is_safe_https_url("file:///etc/passwd"));
    CHECK(user_agent().starts_with("VICTUS-Studio/"));
}

TEST_CASE("library: md5 matches RFC 1321 test vectors") {
    CHECK(md5_hex(bytes("")) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5_hex(bytes("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5_hex(bytes("message digest")) == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5_hex(bytes("12345678901234567890123456789012345678901234567890123456789012345678901234567890")) ==
          "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("library: zip reader inflates deflated packs and skips directories") {
    auto data = fs::read_binary(fs::Path(VICTUS_LIBRARY_DATA_DIR) / "kenney_pack.zip");
    REQUIRE(data.ok());
    // Core alone rejects compressed entries...
    CHECK_FALSE(zip::read(data.value()).ok());
    // ...and accepts them with an inflater.
    zip::ReadOptions o;
    o.inflate = inflate_raw;
    o.skip_directories = true;
    auto entries = zip::read(data.value(), o);
    REQUIRE_MESSAGE(entries.ok(), entries.error().message);
    const auto it = std::find_if(entries->begin(), entries->end(), [](const zip::Entry& e) { return e.name == "Models/GLB format/tree.glb"; });
    REQUIRE(it != entries->end());
    CHECK(it->data.size() == 16 * 40);
    // Decompression-bomb budget.
    o.max_total_bytes = 100;
    CHECK_FALSE(zip::read(data.value(), o).ok());
    // A corrupt stream fails cleanly.
    std::vector<std::uint8_t> junk(64, 0xff);
    CHECK_FALSE(inflate_raw(junk, 100).ok());
}

TEST_CASE("library: every provider declares a licence-safe identity") {
    const auto all = make_all_providers();
    CHECK(all.size() == 7);
    for (const auto& p : all) {
        CHECK_FALSE(p->info().id.empty());
        CHECK_FALSE(p->info().credit.empty());
        CHECK(p->info().homepage.starts_with("https://"));
        CHECK_FALSE(p->info().types.empty());
    }
    CHECK(provider("sketchfab")->info().needs_api_key);
    CHECK(provider("kenney")->info().browse_only);
    CHECK(provider("polyhaven")->info().credit.find("Poly Haven") != std::string::npos); // API terms
}

TEST_CASE("library: Poly Haven search and texture plan") {
    FixtureHttp http;
    http.responses["https://api.polyhaven.com/assets?type=textures"] = R"({
        "red_brick": {"name": "Red Brick", "type": 1, "categories": ["brick", "wall"], "tags": ["red"],
                      "authors": {"Rob Tuytel": "All"}, "download_count": 10},
        "rock_face": {"name": "Rock Face", "type": 1, "categories": ["rock"], "tags": [], "authors": {"Dimitrios": "All"}, "download_count": 99}})";
    http.responses["https://api.polyhaven.com/search?q=brick&t=textures"] = R"({"results": [{"slug": "red_brick", "score": 1}]})";
    http.responses["https://api.polyhaven.com/files/red_brick"] = R"({
        "Diffuse": {"1k": {"jpg": {"url": "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/red_brick/red_brick_diff_1k.jpg", "size": 10, "md5": "aa"}}},
        "nor_gl": {"1k": {"jpg": {"url": "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/red_brick/red_brick_nor_gl_1k.jpg", "size": 10, "md5": "bb"}}},
        "arm": {"1k": {"jpg": {"url": "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/red_brick/red_brick_arm_1k.jpg", "size": 10, "md5": "cc"}}},
        "Displacement": {"1k": {"png": {"url": "https://dl.polyhaven.org/file/ph-assets/Textures/png/1k/red_brick/red_brick_disp_1k.png", "size": 10, "md5": "dd"}}}})";
    auto ph = provider("polyhaven");
    SearchQuery q;
    q.type = static_cast<int>(AssetType::Material);
    auto all = ph->search(http, q);
    REQUIRE(all.ok());
    REQUIRE(all->assets.size() == 2);
    CHECK(all->assets[0].id == "rock_face"); // most downloaded first
    q.text = "brick";
    auto page = ph->search(http, q);
    REQUIRE(page.ok());
    REQUIRE(page->assets.size() == 1);
    const LibraryAsset& a = page->assets[0];
    CHECK(a.name == "Red Brick");
    CHECK(a.author == "Rob Tuytel");
    CHECK(a.licence.id == "CC0-1.0");
    CHECK(a.page_url == "https://polyhaven.com/a/red_brick");
    CHECK(a.thumbnail_url.starts_with("https://cdn.polyhaven.com/"));
    auto plan = ph->plan(http, a, "1k");
    REQUIRE(plan.ok());
    CHECK(plan->make_material);
    CHECK(plan->files.size() == 4);
    CHECK(plan->material.roughness == plan->material.occlusion); // Poly Haven ships glTF ORM ("arm")
    CHECK(plan->material.height.ends_with(".png"));
    CHECK(plan->files[0].md5 == "aa");
    CHECK_FALSE(ph->plan(http, a, "16k").ok());
    // Models are not offered for a texture-only query.
    q.type = static_cast<int>(AssetType::Audio);
    CHECK(ph->search(http, q)->assets.empty());
}

TEST_CASE("library: ambientCG search reads download variants") {
    FixtureHttp http;
    http.responses["https://ambientcg.com/api/v2/full_json?*"] = R"({"numberOfResults": 41, "foundAssets": [
        {"assetId": "Bricks090", "displayName": "Bricks 090", "dataType": "Material", "displayCategory": "Bricks",
         "tags": ["brick", "wall"], "shortLink": "https://ambientcg.com/a/Bricks090",
         "previewImage": {"256-PNG": "https://acg-media.struffelproductions.com/file/ambientCG-Web/media/thumbnail/256-PNG/Bricks090.png"},
         "downloadFolders": {"default": {"downloadFiletypeCategories": {"zip": {"downloads": [
            {"attribute": "1K-JPG", "fullDownloadPath": "https://ambientcg.com/get?file=Bricks090_1K-JPG.zip", "size": 7000000},
            {"attribute": "2K-PNG", "fullDownloadPath": "https://ambientcg.com/get?file=Bricks090_2K-PNG.zip", "size": 9000000},
            {"attribute": "1K-USDZ", "fullDownloadPath": "https://ambientcg.com/get?file=x.usdz", "size": 1}]}}}}}]})";
    auto acg = provider("ambientcg");
    SearchQuery q;
    q.text = "brick";
    auto page = acg->search(http, q);
    REQUIRE(page.ok());
    REQUIRE(page->assets.size() == 1);
    CHECK(http.requested.back().find("q=brick") != std::string::npos);
    CHECK(page->total == 41);
    CHECK(page->has_more);
    const LibraryAsset& a = page->assets[0];
    CHECK(a.variants.size() == 2); // USDZ is not a texture set VICTUS uses
    CHECK(a.category == "Bricks");
    auto plan = acg->plan(http, a, "1K-JPG");
    REQUIRE(plan.ok());
    REQUIRE(plan->files.size() == 1);
    CHECK(plan->files[0].unzip);
    CHECK(plan->material.base_color == "Bricks090_1K-JPG_Color.jpg");
    CHECK(plan->material.normal == "Bricks090_1K-JPG_NormalGL.jpg");
    CHECK_FALSE(acg->plan(http, a, "8K-JPG").ok());
}

TEST_CASE("library: Smithsonian groups file rows per model and skips Draco") {
    FixtureHttp http;
    http.responses["https://3d-api.si.edu/api/v1.0/content/file/search?*"] = R"({"rowCount": 32, "rows": [
        {"title": "Side Chair", "content": {"usage": "Web3D", "quality": "High", "uri": "https://3d-api.si.edu/content/document/3d_package:abc/chair-high.glb", "file_type": "glb", "draco_compressed": true, "model_url": "3d_package:abc"}},
        {"title": "Side Chair", "content": {"usage": "Image2D", "quality": "Thumb", "uri": "https://3d-api.si.edu/content/document/3d_package:abc/scene-image-thumb.jpg", "file_type": "jpg", "model_url": "3d_package:abc"}},
        {"title": "Side Chair", "content": {"usage": "Download3D", "quality": "Low_resolution", "uri": "https://3d-api.si.edu/content/document/3d_package:abc/resources/chair-100k-2048_std_draco.glb", "file_type": "glb", "model_url": "3d_package:abc"}},
        {"title": "Side Chair", "content": {"usage": "Download3D", "quality": "Low_resolution", "uri": "https://3d-api.si.edu/content/document/3d_package:abc/resources/chair-150k-4096_std.glb", "file_type": "glb", "model_url": "3d_package:abc"}},
        {"title": "Draco Only", "content": {"usage": "Download3D", "quality": "Low_resolution", "uri": "https://3d-api.si.edu/content/document/3d_package:def/x_std_draco.glb", "file_type": "glb", "model_url": "3d_package:def"}}]})";
    auto si = provider("smithsonian");
    auto page = si->search(http, SearchQuery{.text = "chair"});
    REQUIRE(page.ok());
    REQUIRE(page->assets.size() == 1);
    const LibraryAsset& a = page->assets[0];
    CHECK(a.id == "abc");
    CHECK(a.thumbnail_url.ends_with("scene-image-thumb.jpg"));
    CHECK(a.licence.installable());
    CHECK(a.licence.notes.find("Trademarks") != std::string::npos);
    auto plan = si->plan(http, a, "");
    REQUIRE(plan.ok());
    CHECK(plan->files[0].url.ends_with("chair-150k-4096_std.glb"));
    CHECK(plan->files[0].relative == "side-chair.glb");
}

TEST_CASE("library: Sketchfab lists only CC0/CC BY and downloads with the user's token") {
    FixtureHttp http;
    const char* results = R"({"next": null, "results": [
        {"uid": "u1", "name": "Old Lantern", "license": {"label": "CC Attribution"}, "user": {"displayName": "Ana"},
         "thumbnails": {"images": [{"url": "https://media.sketchfab.com/a/64.jpeg", "width": 64}, {"url": "https://media.sketchfab.com/a/256.jpeg", "width": 256}, {"url": "https://media.sketchfab.com/a/1024.jpeg", "width": 1024}]},
         "archives": {"glb": {"size": 2048}}, "faceCount": 900},
        {"uid": "u2", "name": "NC Thing", "license": {"label": "CC Attribution-NonCommercial"}, "user": {"displayName": "B"}, "thumbnails": {"images": []}},
        {"uid": "u3", "name": "Adult", "isAgeRestricted": true, "license": {"label": "CC Attribution"}, "user": {"displayName": "C"}, "thumbnails": {"images": []}}]})";
    http.responses["https://api.sketchfab.com/v3/search?*"] = results;
    auto sf = provider("sketchfab");
    auto page = sf->search(http, SearchQuery{.text = "lantern"});
    REQUIRE(page.ok());
    // Both licence queries return the same canned list; only the CC BY model survives each.
    REQUIRE(page->assets.size() == 2);
    const LibraryAsset& a = page->assets[0];
    CHECK(a.id == "u1");
    CHECK(a.thumbnail_url.ends_with("256.jpeg"));
    CHECK(a.licence.attribution_required);
    CHECK(a.requires_account);
    CHECK(a.attribution().find("Ana") != std::string::npos);
    // No token: a clear error, nothing requested.
    const std::size_t before = http.requested.size();
    auto none = sf->plan(http, a, "");
    CHECK_FALSE(none.ok());
    CHECK(none.error().message.find("token") != std::string::npos);
    CHECK(http.requested.size() == before);
    http.responses["https://api.sketchfab.com/v3/models/u1/download"] = R"({"glb": {"url": "https://sketchfab-prod-media.s3.amazonaws.com/archives/u1.glb?X-Amz-Signature=1", "size": 2048}})";
    sf->set_api_key("secret-token");
    auto plan = sf->plan(http, a, "");
    REQUIRE(plan.ok());
    CHECK(plan->files[0].headers.empty()); // the token never goes to the signed storage URL
    CHECK(plan->files[0].relative == "old-lantern.glb");
    REQUIRE_FALSE(http.headers.back().empty());
    CHECK(http.headers.back()[0].value == "Token secret-token");
}

TEST_CASE("library: BigSoundBank search page parsing") {
    FixtureHttp http;
    http.responses["https://bigsoundbank.com/search?q=door"] =
        "<div class='resultats'><div class='resultat_0'><h2 style='x'><a href='/door-s0015.html'>Door</a><a href=\"/account/\">"
        "</a></h2><div><div class=\"lecteur\"><audio id=\"MyAudio0015\"><source src=\"/UPLOAD/mp3/0015.mp3\" type=\"audio/mpeg\">"
        "<source src=\"/UPLOAD/ogg/0015.ogg\"></audio><button><svg></svg></button></div></div><div>Light door: knock (x5). "
        "<a href='/categories.html'>UCS Category</a>: <a href='/search?CatID=DOORKnck'>DOORKnck</a>. Length: 00:05.</div></div></div>"
        "<div class='resultats'><div class='resultat_1'><h2><a href='/apartment-door-s1704.html'>Apartment Door, Closing<span>&nbsp;#1</span></a></h2>"
        "<audio><source src=\"/UPLOAD/mp3/1704.mp3\"></audio></div></div>";
    auto bsb = provider("bigsoundbank");
    auto page = bsb->search(http, SearchQuery{.text = "door"});
    REQUIRE(page.ok());
    REQUIRE(page->assets.size() == 2);
    CHECK(page->assets[0].id == "0015");
    CHECK(page->assets[0].name == "Door");
    CHECK(page->assets[0].category == "DOORKnck");
    CHECK(page->assets[0].page_url == "https://bigsoundbank.com/door-s0015.html");
    CHECK(page->assets[1].id == "1704");
    auto plan = bsb->plan(http, page->assets[0], "");
    REQUIRE(plan.ok());
    CHECK(plan->files[0].url == "https://bigsoundbank.com/UPLOAD/ogg/0015.ogg");
    LibraryAsset evil = page->assets[0];
    evil.id = "../x";
    CHECK_FALSE(bsb->plan(http, evil, "").ok());
}

TEST_CASE("library: NPS sounds install only recordings credited to the NPS") {
    FixtureHttp http;
    http.responses["https://www.nps.gov/subjects/sound/gallery.htm"] =
        "<h3><strong>Reptiles</strong></h3><ul><li><a href=\"/subjects/sound/sounds-alligator.htm\" id=\"x\">Alligator</a></li></ul>"
        "<h3><strong>Birds</strong></h3><ul><li><a href=\"/subjects/sound/sounds-bald-eagle.htm\" id=\"y\">Bald Eagle</a></li>"
        "<li><a href=\"/subjects/sound/sounds-loon.htm\">Common Loon</a></li></ul>";
    http.responses["https://www.nps.gov/subjects/sound/sounds-alligator.htm"] =
        "<script>element.setAttribute('href', 'https://www.nps.gov/nps-audiovideo/legacy/mp3/nri/avElement/nri-AlligatorEVER1.mp3');</script>"
        "<dl><dt>Credit / Author:</dt>\n<dd>NPS</dd></dl>";
    http.responses["https://www.nps.gov/subjects/sound/sounds-loon.htm"] =
        "<script>element.setAttribute('href', 'https://www.nps.gov/media/loon.mp3');</script><dt>Credit / Author:</dt><dd>Jane Doe</dd>";
    auto nps = provider("nps");
    auto all = nps->search(http, SearchQuery{});
    REQUIRE(all.ok());
    REQUIRE(all->assets.size() == 3);
    CHECK(all->assets[0].category == "Reptiles");
    CHECK(all->assets[2].category == "Birds");
    auto birds = nps->search(http, SearchQuery{.text = "birds"});
    CHECK(birds->assets.size() == 2);
    auto plan = nps->plan(http, all->assets[0], "");
    REQUIRE(plan.ok());
    CHECK(plan->files[0].url.ends_with("nri-AlligatorEVER1.mp3"));
    auto loon = nps->plan(http, all->assets[2], "");
    REQUIRE_FALSE(loon.ok());
    CHECK(loon.error().message.find("Jane Doe") != std::string::npos);
}

TEST_CASE("library: Kenney catalogue is browse-only") {
    FixtureHttp http;
    http.responses["https://kenney.nl/assets/category:3D?search=nature"] =
        "<div class='asset'><a href='https://kenney.nl/assets/nature-kit'><div class='cover' style='background-image:url(\""
        "https://kenney.nl/media/pages/assets/nature-kit/656a90532f-1677698896/sample-400x.png\")'></div></a>"
        "<h2><a href='https://kenney.nl/assets/nature-kit'>Nature Kit</a></h2><span><a href='https://kenney.nl/assets/category:3D'>3D</a>"
        "<a href='https://kenney.nl/assets/series:Kits'>Kits</a></span></div>"
        "<a href='https://kenney.nl/assets/page:2?search=nature'>2</a>";
    auto k = provider("kenney");
    SearchQuery q;
    q.text = "nature";
    q.type = static_cast<int>(AssetType::Model);
    auto page = k->search(http, q);
    REQUIRE(page.ok());
    REQUIRE(page->assets.size() == 1);
    CHECK(page->has_more);
    const LibraryAsset& a = page->assets[0];
    CHECK(a.id == "nature-kit");
    CHECK(a.type == AssetType::Model);
    CHECK(a.thumbnail_url.ends_with("sample-400x.png"));
    auto plan = k->plan(http, a, "");
    REQUIRE_FALSE(plan.ok());
    CHECK(plan.error().code == ErrorCode::Unsupported);
}

TEST_CASE("library: assets round-trip through JSON (provenance records)") {
    LibraryAsset a;
    a.provider = "sketchfab";
    a.id = "u1";
    a.name = "Lantern";
    a.type = AssetType::Model;
    a.author = "Ana";
    a.page_url = "https://sketchfab.com/3d-models/u1";
    a.licence = licence_cc_by();
    a.tags = {"light"};
    a.variants = {{"glb", "GLB", 10}};
    auto back = LibraryAsset::from_json(a.to_json());
    REQUIRE(back.ok());
    CHECK(back->key() == "sketchfab:u1");
    CHECK(back->licence.attribution_required);
    CHECK(back->tags == a.tags);
    CHECK(back->attribution() == a.attribution());
    CHECK_FALSE(LibraryAsset::from_json(json::parse("{}").value()).ok());
}
