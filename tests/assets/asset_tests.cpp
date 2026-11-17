// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "gltf_writer.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/hash.hpp"

#include <filesystem>
#include <format>

using namespace victus;
using namespace victus::assets;

namespace {

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_bytes(const fs::Path& p, const std::vector<std::uint8_t>& b) {
    std::filesystem::create_directories(p.parent_path());
    REQUIRE(fs::write_atomic(p, b).ok());
}
void write_text(const fs::Path& p, const std::string& s) {
    std::filesystem::create_directories(p.parent_path());
    REQUIRE(fs::write_text_atomic(p, s).ok());
}
std::vector<std::uint8_t> png(std::uint32_t size, std::uint32_t a, std::uint32_t b) {
    auto bytes = render::encode_png(render::make_checker(size, 4, a, b));
    REQUIRE(bytes.ok());
    return *bytes;
}

void check_fixture_model(const CookedModel& m) {
    REQUIRE(m.meshes.size() == 1);
    CHECK(m.meshes[0].data.vertices.size() == 4);
    CHECK(m.meshes[0].data.indices.size() == 6);
    // Tangents were generated (source has none) and are unit length.
    CHECK(math::length(m.meshes[0].data.vertices[0].tangent.xyz()) == doctest::Approx(1.0f).epsilon(1e-3));
    REQUIRE(m.materials.size() == 1);
    CHECK(m.materials[0].name == "Brick");
    CHECK(m.materials[0].base_color.r == doctest::Approx(0.8f));
    CHECK(m.materials[0].metallic == doctest::Approx(0.25f));
    CHECK(m.materials[0].roughness == doctest::Approx(0.75f));
    CHECK(m.materials[0].double_sided);
    REQUIRE(m.nodes.size() == 2);
    CHECK(m.nodes[0].name == "Root");
    CHECK(m.nodes[0].parent == -1);
    CHECK(m.nodes[0].translation.x == doctest::Approx(2.0f));
    CHECK(m.nodes[1].name == "Child");
    CHECK(m.nodes[1].parent == 0);
    CHECK(m.nodes[1].scale.y == doctest::Approx(2.0f));
    CHECK(m.nodes[1].meshes == std::vector<std::uint32_t>{0});
}

} // namespace

TEST_CASE("assets: cooked texture/model round trip and corruption rejection") {
    CookedTexture tex;
    tex.name = "checker";
    tex.srgb = false;
    tex.mips = render::build_mip_chain(render::make_checker(16, 4, 0xff0000ff, 0xffffffff), false);
    CookedHeader h;
    h.key = 0x1234;
    h.dependencies.push_back({"assets/x.bin", 99});
    const auto bytes = serialize(h, tex);
    auto back = deserialize_texture(bytes);
    REQUIRE(back.ok());
    CHECK(back->name == "checker");
    CHECK_FALSE(back->srgb);
    CHECK(back->mips.size() == 5);
    CHECK(back->mips[0].pixels == tex.mips[0].pixels);
    auto header = read_header(bytes);
    REQUIRE(header.ok());
    CHECK(header->key == 0x1234);
    CHECK(header->kind == AssetKind::Texture);
    REQUIRE(header->dependencies.size() == 1);
    CHECK(header->dependencies[0].hash == 99);
    CHECK_FALSE(deserialize_model(bytes).ok()); // wrong kind

    // Every truncation and a sweep of single-byte corruptions must fail cleanly (run under ASan).
    for (std::size_t n = 0; n < bytes.size(); n += 7) {
        CHECK_FALSE(deserialize_texture(std::span(bytes.data(), n)).ok());
    }
    std::size_t rejected = 0;
    for (std::size_t i = 0; i < 64; ++i) {
        auto copy = bytes;
        copy[i] ^= 0xA5;
        if (!deserialize_texture(copy).ok()) ++rejected;
    }
    CHECK(rejected > 0);

    CookedModel model;
    model.materials.push_back({});
    model.meshes.push_back({"cube", render::make_cube(), 0});
    model.nodes.push_back({"root", -1, {1, 2, 3}, {}, {1, 1, 1}, {0}});
    model.textures.push_back(tex);
    model.materials[0].base_color_texture = 0;
    const auto mbytes = serialize(h, model);
    auto mback = deserialize_model(mbytes);
    REQUIRE(mback.ok());
    CHECK(mback->meshes[0].data.vertices.size() == model.meshes[0].data.vertices.size());
    CHECK(mback->nodes[0].translation.z == 3.0f);
    CHECK(mback->materials[0].base_color_texture == 0);
    CHECK(mback->triangle_count() == 12);
    for (std::size_t n = 0; n < mbytes.size(); n += 97) CHECK_FALSE(deserialize_model(std::span(mbytes.data(), n)).ok());
}

TEST_CASE("assets: glTF import from .gltf (external buffer+image), data URIs and .glb") {
    TempDir dir("victus_gltf");
    test::GltfFixture fx;
    write_bytes(dir.path / "quad.bin", fx.bin);
    write_bytes(dir.path / "tex" / "albedo.png", png(32, 0xff0000ff, 0xff00ff00));
    fx.add_texture("tex/albedo.png");
    fx.set_buffer_uri("quad.bin");
    write_text(dir.path / "quad.gltf", json::stringify(fx.doc));

    ImportResult result;
    auto m = import_gltf(dir.path / "quad.gltf", {}, dir.path, &result);
    REQUIRE_MESSAGE(m.ok(), (m.ok() ? "" : m.error().message));
    check_fixture_model(*m);
    REQUIRE(m->textures.size() == 1);
    CHECK(m->textures[0].srgb);
    CHECK(m->textures[0].mips[0].width == 32);
    CHECK(m->textures[0].mips.size() == 6);
    CHECK(m->materials[0].base_color_texture == 0);
    CHECK(result.dependencies.size() == 2); // quad.bin + albedo.png

    // Data URIs for buffer and image.
    test::GltfFixture fx2;
    fx2.add_texture("data:image/png;base64," + test::base64(png(8, 0xffffffff, 0xff000000)));
    write_text(dir.path / "embedded.gltf", fx2.gltf_with_data_uri());
    ImportResult r2;
    auto m2 = import_gltf(dir.path / "embedded.gltf", {}, dir.path, &r2);
    REQUIRE_MESSAGE(m2.ok(), (m2.ok() ? "" : m2.error().message));
    check_fixture_model(*m2);
    CHECK(m2->textures.size() == 1);
    CHECK(r2.dependencies.empty());

    // Binary glTF with scale setting.
    test::GltfFixture fx3;
    write_bytes(dir.path / "quad.glb", fx3.glb());
    ModelImportSettings s;
    s.scale = 0.5f;
    auto m3 = import_gltf(dir.path / "quad.glb", s, dir.path);
    REQUIRE_MESSAGE(m3.ok(), (m3.ok() ? "" : m3.error().message));
    CHECK(m3->nodes[0].translation.x == doctest::Approx(1.0f));
    CHECK(m3->nodes[0].scale.x == doctest::Approx(0.5f));
}

TEST_CASE("assets: glTF importer rejects escapes, missing files and garbage") {
    TempDir dir("victus_gltf_bad");
    test::GltfFixture fx;
    write_bytes(dir.path / "outside.bin", fx.bin);
    fx.set_buffer_uri("../outside.bin");
    write_text(dir.path / "sub" / "escape.gltf", json::stringify(fx.doc));
    auto m = import_gltf(dir.path / "sub" / "escape.gltf", {}, dir.path / "sub");
    REQUIRE_FALSE(m.ok());
    CHECK(m.error().message.find("escapes") != std::string::npos);

    fx.set_buffer_uri("missing.bin");
    write_text(dir.path / "missing.gltf", json::stringify(fx.doc));
    CHECK_FALSE(import_gltf(dir.path / "missing.gltf", {}, dir.path).ok());
    write_text(dir.path / "garbage.gltf", "{ this is not json");
    CHECK_FALSE(import_gltf(dir.path / "garbage.gltf", {}, dir.path).ok());
    write_bytes(dir.path / "garbage.glb", {0x67, 0x6C, 0x54, 0x46, 2, 0, 0, 0, 0xff, 0xff, 0, 0});
    CHECK_FALSE(import_gltf(dir.path / "garbage.glb", {}, dir.path).ok());

    // Out-of-range index is caught (validation or importer), never read out of bounds.
    test::GltfFixture bad;
    bad.bin[128] = 200;
    write_bytes(dir.path / "badidx.bin", bad.bin);
    bad.set_buffer_uri("badidx.bin");
    write_text(dir.path / "badidx.gltf", json::stringify(bad.doc));
    CHECK_FALSE(import_gltf(dir.path / "badidx.gltf", {}, dir.path).ok());
}

TEST_CASE("assets: database metas, stable UUIDs, incremental cooking and dependency tracking") {
    TempDir project("victus_project");
    const fs::Path assets = project.path / "assets";
    write_bytes(assets / "textures" / "wall.png", png(16, 0xff112233, 0xff445566));
    write_bytes(assets / "textures" / "wall_n.png", png(16, 0xffff8080, 0xff8080ff));
    test::GltfFixture fx;
    write_bytes(assets / "models" / "quad.bin", fx.bin);
    fx.set_buffer_uri("quad.bin");
    write_text(assets / "models" / "quad.gltf", json::stringify(fx.doc));
    write_text(assets / "readme.txt", "hello");

    RefreshReport report;
    auto opened = AssetDatabase::open(project.path, &report);
    REQUIRE(opened.ok());
    auto& db = **opened;
    CHECK(report.added.size() == 5);
    CHECK(std::filesystem::exists(assets / "textures" / "wall.png.meta"));
    const AssetRecord* wall = db.find_path("textures/wall.png");
    const AssetRecord* normal = db.find_path("textures/wall_n.png");
    const AssetRecord* model = db.find_path("models/quad.gltf");
    REQUIRE(wall);
    REQUIRE(normal);
    REQUIRE(model);
    CHECK(wall->kind == AssetKind::Texture);
    CHECK(wall->settings["srgb"].as_bool());
    CHECK_FALSE(normal->settings["srgb"].as_bool(true)); // naming convention → linear
    CHECK(model->kind == AssetKind::Model);
    CHECK(db.find_path("readme.txt")->kind == AssetKind::Unknown);
    const Uuid wall_id = wall->uuid, model_id = model->uuid;

    jobs::JobSystem js(2);
    ImportReport ir = db.import_all(&js);
    CHECK(ir.imported == 3);
    CHECK(ir.failed == 0);
    ir = db.import_all(&js);
    CHECK(ir.imported == 0);
    CHECK(ir.up_to_date == 3);

    auto tex = db.load_texture(wall_id);
    REQUIRE(tex.ok());
    CHECK(tex->mips[0].width == 16);
    auto mdl = db.load_model(model_id);
    REQUIRE(mdl.ok());
    check_fixture_model(*mdl);

    // UUIDs survive reopening.
    auto reopened = AssetDatabase::open(project.path);
    REQUIRE(reopened.ok());
    CHECK((*reopened)->find_path("textures/wall.png")->uuid == wall_id);

    // Changing a source re-cooks only that asset.
    write_bytes(assets / "textures" / "wall.png", png(16, 0xff000000, 0xffffffff));
    CHECK_FALSE(*db.is_up_to_date(wall_id));
    ir = db.import_all();
    CHECK(ir.imported == 1);
    CHECK(ir.up_to_date == 2);

    // Changing an external dependency (the .bin) re-cooks the model.
    fx.bin[0] ^= 1;
    write_bytes(assets / "models" / "quad.bin", fx.bin);
    CHECK_FALSE(*db.is_up_to_date(model_id));
    CHECK(*db.import(model_id));

    // Settings changes re-cook and are persisted in the .meta.
    json::Value settings = db.find(wall_id)->settings;
    settings.set("max_size", 8);
    REQUIRE(db.set_settings(wall_id, settings).ok());
    auto small = db.load_texture(wall_id);
    REQUIRE(small.ok());
    CHECK(small->mips[0].width == 8);
    CHECK(fs::read_text(assets / "textures" / "wall.png.meta")->find("\"max_size\": 8") != std::string::npos);

    // A corrupt cache entry is detected as stale and rebuilt.
    write_bytes(db.cooked_path(wall_id), {1, 2, 3});
    CHECK_FALSE(*db.is_up_to_date(wall_id));
    CHECK(db.load_texture(wall_id).ok());

    // Copying a file together with its .meta yields a fresh UUID for the copy.
    std::filesystem::copy_file(assets / "textures" / "wall.png", assets / "textures" / "wall_copy.png");
    std::filesystem::copy_file(assets / "textures" / "wall.png.meta", assets / "textures" / "wall_copy.png.meta");
    // Deleting a source leaves its .meta reported as missing.
    std::filesystem::remove(assets / "readme.txt");
    // Invalid metas are reported and never overwritten.
    write_bytes(assets / "broken.png", png(4, 0xffffffff, 0xff000000));
    write_text(assets / "broken.png.meta", "{ not json");
    report = db.refresh();
    REQUIRE(report.reassigned.size() == 1);
    CHECK(report.reassigned[0] == "textures/wall_copy.png");
    CHECK(db.find_path("textures/wall.png")->uuid == wall_id);
    CHECK(db.find_path("textures/wall_copy.png")->uuid != wall_id);
    CHECK(report.missing == std::vector<std::string>{"readme.txt"});
    REQUIRE(report.errors.size() == 1);
    CHECK(*fs::read_text(assets / "broken.png.meta") == "{ not json");
    CHECK(db.find_path("broken.png") == nullptr);
}

TEST_CASE("assets: material instances inherit, override, round trip and reject broken chains") {
    // JSON: an instance records exactly the keys it sets and writes only those back.
    auto doc = json::parse(R"({"victus_material": 1, "parent": "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f003",
                               "base_color": [0, 0, 1, 1], "base_color_texture": null})");
    REQUIRE(doc.ok());
    auto inst = MaterialAsset::from_json(*doc);
    REQUIRE(inst.ok());
    CHECK(inst->is_instance());
    CHECK(inst->overrides == (field_bit(MaterialField::BaseColor) | field_bit(MaterialField::BaseColorTexture)));
    const json::Value written = inst->to_json();
    CHECK(written.size() == 4); // victus_material, parent, base_color, base_color_texture
    CHECK(written.find("roughness") == nullptr);
    auto back = MaterialAsset::from_json(written);
    REQUIRE(back.ok());
    CHECK(*back == *inst);
    auto plain = MaterialAsset::from_json(*json::parse(R"({"victus_material": 1})"));
    REQUIRE(plain.ok());
    CHECK(plain->overrides == kAllMaterialFields); // standalone materials own every field
    CHECK_FALSE(MaterialAsset::from_json(*json::parse(R"({"victus_material": 1, "parent": 3})")).ok());

    TempDir project("victus_matinst");
    const fs::Path dir = project.path / "assets";
    std::filesystem::create_directories(dir);
    auto opened = AssetDatabase::open(project.path);
    REQUIRE(opened.ok());
    AssetDatabase& db = **opened;
    auto write = [&](const char* name, const MaterialAsset& m) {
        REQUIRE(save_material_file(dir / name, m).ok());
        (void)db.refresh();
        return db.find_path(name)->uuid;
    };
    MaterialAsset base;
    base.base_color = {1, 0, 0, 1};
    base.roughness = 0.2f;
    base.uv_scale = {4, 4};
    base.base_color_texture = Uuid::generate();
    const Uuid base_id = write("base.vmat", base);
    MaterialAsset child;
    child.parent = base_id;
    child.base_color = {0, 0, 1, 1};
    child.overrides = field_bit(MaterialField::BaseColor);
    const Uuid child_id = write("child.vmat", child);
    MaterialAsset grandchild;
    grandchild.parent = child_id;
    grandchild.metallic = 1.0f;
    grandchild.base_color_texture = {};
    grandchild.overrides = field_bit(MaterialField::Metallic) | field_bit(MaterialField::BaseColorTexture);
    const Uuid gc_id = write("grandchild.vmat", grandchild);

    auto r = db.resolve_material(gc_id);
    REQUIRE(r.ok());
    CHECK(r->base_color == math::Color{0, 0, 1, 1}); // from child
    CHECK(r->roughness == doctest::Approx(0.2f));    // from base
    CHECK(r->uv_scale == math::Vec2{4, 4});
    CHECK(r->metallic == doctest::Approx(1.0f));     // own
    CHECK(r->base_color_texture.is_nil());           // overridden to "none"
    CHECK(r->parent == child_id);                    // the leaf's own parent and mask are kept
    CHECK(r->overrides == grandchild.overrides);
    // Saving the file content (not the resolved values) is untouched by inheritance.
    auto raw = db.load_material(gc_id);
    REQUIRE(raw.ok());
    CHECK(raw->base_color == math::Color{1, 1, 1, 1});

    // Parent edits propagate.
    base.roughness = 0.7f;
    (void)write("base.vmat", base);
    CHECK(db.resolve_material(gc_id)->roughness == doctest::Approx(0.7f));

    // Cycles, missing parents and overly deep chains are errors naming the files.
    MaterialAsset loop = base;
    loop.parent = gc_id;
    loop.overrides = 0;
    (void)write("base.vmat", loop);
    auto cyc = db.resolve_material(gc_id);
    REQUIRE_FALSE(cyc.ok());
    CHECK(cyc.error().message.find("cycle") != std::string::npos);
    CHECK(cyc.error().message.find("grandchild.vmat") != std::string::npos);
    std::filesystem::remove(dir / "base.vmat");
    std::filesystem::remove(dir / "base.vmat.meta");
    (void)db.refresh();
    auto missing = db.resolve_material(gc_id);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("child.vmat: parent material") != std::string::npos);

    MaterialAsset root;
    Uuid prev = write("chain0.vmat", root);
    for (int i = 1; i <= static_cast<int>(kMaxMaterialDepth); ++i) {
        MaterialAsset link;
        link.parent = prev;
        link.overrides = 0;
        prev = write(std::format("chain{}.vmat", i).c_str(), link);
    }
    auto deep = db.resolve_material(prev);
    REQUIRE_FALSE(deep.ok());
    CHECK(deep.error().code == ErrorCode::LimitExceeded);
}
