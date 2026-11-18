// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "gltf_writer.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/hash.hpp"
#include "victus/render/environment_map.hpp"

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
    CHECK(kind_for_extension(".vbt") == AssetKind::BehaviorTree);
    CHECK(std::string(to_string(AssetKind::BehaviorTree)) == "behavior_tree");
    CHECK(kind_for_extension(".vseq") == AssetKind::Sequence);
    CHECK(std::string(to_string(AssetKind::Sequence)) == "sequence");
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

TEST_CASE("assets: material emissive accepts 3 or 4 numbers") {
    // Regression (ASan, parser fuzzing): a 4-element emissive array overflowed a 3-float buffer.
    for (const char* text : {R"({"victus_material": 1, "emissive": [0.5, 0.25, 1]})",
                             R"({"victus_material": 1, "emissive": [0.5, 0.25, 1, 7]})"}) {
        auto doc = json::parse(text);
        REQUIRE(doc.ok());
        auto m = MaterialAsset::from_json(*doc);
        REQUIRE(m.ok());
        CHECK(m->emissive.r == doctest::Approx(0.5f));
        CHECK(m->emissive.g == doctest::Approx(0.25f));
        CHECK(m->emissive.b == doctest::Approx(1.0f));
        CHECK(m->emissive.a == doctest::Approx(1.0f));
    }
    auto bad = json::parse(R"({"victus_material": 1, "emissive": [1, 2, 3, 4, 5]})");
    REQUIRE(bad.ok());
    CHECK_FALSE(MaterialAsset::from_json(*bad).ok());
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

TEST_CASE("material blend and coverage fields round trip and inherit") {
    auto doc = json::parse(R"({"victus_material": 1, "blend": "alpha", "vertex_alpha": "coverage_mask", "coverage_response": 0.25})");
    REQUIRE(doc.ok());
    auto m = MaterialAsset::from_json(*doc);
    REQUIRE(m.ok());
    CHECK(m->blend == MaterialBlendMode::Alpha);
    CHECK(m->vertex_alpha_coverage_mask);
    CHECK(m->coverage_response == doctest::Approx(0.25f));
    auto back = MaterialAsset::from_json(m->to_json());
    REQUIRE(back.ok());
    CHECK(back->blend == MaterialBlendMode::Alpha);
    MaterialAsset parent;
    parent.blend = MaterialBlendMode::Additive;
    MaterialAsset child = *m;
    child.parent = Uuid::generate();
    child.overrides = field_bit(MaterialField::CoverageResponse);
    const MaterialAsset merged = apply_instance(parent, child);
    CHECK(merged.blend == MaterialBlendMode::Additive); // inherited
    CHECK(merged.coverage_response == doctest::Approx(0.25f));
    CHECK_FALSE(MaterialAsset::from_json(*json::parse(R"({"victus_material": 1, "blend": "glass"})")).ok());
}

TEST_CASE("assets: HDR textures cook to half floats with SH irradiance and an LDR preview") {
    TempDir dir("victus_hdr_cook");
    render::ImageF env;
    env.width = 64;
    env.height = 32;
    env.pixels.resize(64 * 32 * 4);
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            float* p = &env.pixels[(y * 64 + x) * 4];
            p[0] = y < 16 ? 4.0f : 0.1f;
            p[1] = p[2] = y < 16 ? 2.0f : 0.1f;
            p[3] = 1;
        }
    auto bytes = render::encode_hdr(env);
    REQUIRE(bytes.ok());
    const fs::Path file = dir.path / "sky.hdr";
    write_bytes(file, *bytes);
    TextureImportSettings settings;
    settings.max_size = 32; // forces a 32x16 downsample
    auto tex = import_texture(file, settings);
    REQUIRE(tex.ok());
    CHECK(tex->hdr);
    CHECK(tex->hdr_width == 32);
    CHECK(tex->hdr_height == 16);
    REQUIRE(tex->hdr_pixels.size() == 32 * 16 * 4);
    CHECK(render::half_to_float(tex->hdr_pixels[0]) == doctest::Approx(4.0f).epsilon(0.02));
    REQUIRE_FALSE(tex->mips.empty());
    CHECK(tex->mips[0].pixels[0] > tex->mips[0].pixels[(tex->mips[0].pixels.size() / 4 - 1) * 4]); // tonemapped preview
    render::SphericalHarmonics9 sh;
    sh.coefficients = tex->sh;
    CHECK(sh.diffuse({0, 1, 0}).x > sh.diffuse({0, -1, 0}).x * 5.0f);

    CookedHeader header;
    header.kind = AssetKind::Texture;
    const auto blob = serialize(header, *tex);
    auto back = deserialize_texture(blob);
    REQUIRE(back.ok());
    CHECK(back->hdr);
    CHECK(back->hdr_pixels == tex->hdr_pixels);
    CHECK(back->sh[0].x == doctest::Approx(tex->sh[0].x));
    const auto img = back->hdr_image();
    CHECK(img.width == 32);
    CHECK(img.at(0, 0).x == doctest::Approx(4.0f).epsilon(0.02));
    // Truncated HDR payloads are rejected.
    std::vector<std::uint8_t> cut(blob.begin(), blob.end() - 40);
    CHECK_FALSE(deserialize_texture(cut).ok());
}

namespace {
json::Value sparse_block(int count, int indices_view, int values_view) {
    json::Value idx = json::Value::object();
    idx.set("bufferView", indices_view);
    idx.set("componentType", 5121); // UNSIGNED_BYTE
    json::Value val = json::Value::object();
    val.set("bufferView", values_view);
    json::Value sp = json::Value::object();
    sp.set("count", count);
    sp.set("indices", idx);
    sp.set("values", val);
    return sp;
}
json::Value buffer_view(std::uint64_t offset, std::uint64_t length, int stride = 0) {
    json::Value v = json::Value::object();
    v.set("buffer", 0);
    v.set("byteOffset", offset);
    v.set("byteLength", length);
    if (stride) v.set("byteStride", stride);
    return v;
}
void append_bytes(std::vector<std::uint8_t>& bin, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    bin.insert(bin.end(), b, b + n);
    while (bin.size() % 4) bin.push_back(0);
}
} // namespace

// cgltf <= 1.15 advisories CVE-2026-112018/112019/32845/75148: crafted accessors must be rejected by
// the importer's own checked validation, and valid sparse accessors over strided views must read
// tightly packed sparse values.
TEST_CASE("assets: glTF importer hardening against crafted accessors (cgltf advisories)") {
    TempDir dir("victus_gltf_crafted");
    auto import_doc = [&](const test::GltfFixture& fx, const char* name) {
        test::GltfFixture copy = fx;
        write_text(dir.path / name, copy.gltf_with_data_uri());
        ImportResult res;
        ModelImportSettings plain;
        plain.optimize_meshes = false;
        return import_gltf(dir.path / name, plain, dir.path, &res);
    };

    SUBCASE("accessor counts that overflow the bounds check are rejected") {
        for (double count : {0.0, 4294967296.0, 1099511627776.0, 268435456.0}) {
            test::GltfFixture fx;
            json::Value acc = fx.doc["accessors"];
            json::Value pos = acc[0];
            pos.set("count", count);
            json::Array list = acc.as_array();
            list[0] = pos;
            fx.doc.set("accessors", json::Value(list));
            auto m = import_doc(fx, "overflow.gltf");
            REQUIRE_FALSE(m.ok());
            if (count > 0) CHECK(m.error().message.find("accessor 0") != std::string::npos);
        }
    }

    SUBCASE("sparse indices outside the accessor are rejected") {
        test::GltfFixture fx;
        const std::uint8_t indices[] = {7};
        const float values[] = {9, 9, 9};
        const auto idx_at = fx.bin.size();
        append_bytes(fx.bin, indices, sizeof indices);
        const auto val_at = fx.bin.size();
        append_bytes(fx.bin, values, sizeof values);
        json::Array views = fx.doc["bufferViews"].as_array();
        views.push_back(buffer_view(idx_at, 1));
        views.push_back(buffer_view(val_at, 12));
        fx.doc.set("bufferViews", json::Value(views));
        json::Array accs = fx.doc["accessors"].as_array();
        accs[0].set("sparse", sparse_block(1, 4, 5));
        fx.doc.set("accessors", json::Value(accs));
        auto m = import_doc(fx, "sparse_oob.gltf");
        REQUIRE_FALSE(m.ok());
        CHECK(m.error().message.find("sparse index 7") != std::string::npos);
    }

    SUBCASE("sparse values over a strided base view are read tightly packed") {
        test::GltfFixture fx;
        // Positions re-laid out with a 16-byte stride; two sparse replacements for vertices 1 and 2.
        const float base[] = {-0.5f, 0, -0.5f, 0, 0.5f, 0, -0.5f, 0, 0.5f, 0, 0.5f, 0, -0.5f, 0, 0.5f, 0};
        const std::uint8_t indices[] = {1, 2};
        const float values[] = {1, 2, 3, 4, 5, 6};
        const auto base_at = fx.bin.size();
        append_bytes(fx.bin, base, sizeof base);
        const auto idx_at = fx.bin.size();
        append_bytes(fx.bin, indices, sizeof indices);
        const auto val_at = fx.bin.size();
        append_bytes(fx.bin, values, sizeof values);
        json::Array views = fx.doc["bufferViews"].as_array();
        views.push_back(buffer_view(base_at, sizeof base, 16));
        views.push_back(buffer_view(idx_at, 2));
        views.push_back(buffer_view(val_at, sizeof values));
        fx.doc.set("bufferViews", json::Value(views));
        json::Array accs = fx.doc["accessors"].as_array();
        accs[0].set("bufferView", 4);
        accs[0].set("sparse", sparse_block(2, 5, 6));
        accs[0].set("min", json::Value(json::Array{-0.5, 0.0, -0.5}));
        accs[0].set("max", json::Value(json::Array{4.0, 5.0, 6.0}));
        fx.doc.set("accessors", json::Value(accs));
        auto m = import_doc(fx, "sparse_strided.gltf");
        REQUIRE(m.ok());
        REQUIRE(m->meshes.size() == 1);
        const auto& v = m->meshes[0].data.vertices;
        REQUIRE(v.size() == 4);
        CHECK(v[0].position.x == doctest::Approx(-0.5f));
        CHECK(v[1].position.x == doctest::Approx(1.0f));
        CHECK(v[1].position.z == doctest::Approx(3.0f));
        CHECK(v[2].position.x == doctest::Approx(4.0f));
        CHECK(v[2].position.y == doctest::Approx(5.0f));
        CHECK(v[2].position.z == doctest::Approx(6.0f));
        CHECK(v[3].position.z == doctest::Approx(0.5f));
    }
}
