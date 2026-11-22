// SPDX-License-Identifier: Apache-2.0
// FBX and OBJ import through ufbx (ADR-0173): meshes, materials, embedded textures, skins with
// unweighted vertices, blend shapes and their animation, lights, cameras, OBJ + MTL + texture,
// sandboxing and the asset database path.
#include "doctest.h"
#include "victus/assets/database.hpp"
#include "victus/assets/importers.hpp"
#include "victus/core/jobs.hpp"
#include "victus/render/image.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace victus;
using namespace victus::assets;

namespace {

const fs::Path kDir = fs::Path(VICTUS_FBX_DIR);

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

void write_text(const fs::Path& p, const std::string& s) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

math::Mat4 world(const CookedModel& m, std::int32_t node) {
    math::Mat4 w;
    for (; node >= 0; node = m.nodes[static_cast<std::size_t>(node)].parent) {
        const ModelNode& n = m.nodes[static_cast<std::size_t>(node)];
        w = math::Mat4::trs(n.translation, n.rotation, n.scale) * w;
    }
    return w;
}

struct Bounds {
    math::Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    math::Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    void add(math::Vec3 p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
};

Bounds world_bounds(const CookedModel& m, std::size_t node_index) {
    Bounds b;
    const math::Mat4 w = world(m, static_cast<std::int32_t>(node_index));
    for (const std::uint32_t mi : m.nodes[node_index].meshes)
        for (const auto& v : m.meshes[mi].data.vertices) b.add(w.transform_point(v.position));
    return b;
}

std::int32_t find_node(const CookedModel& m, std::string_view name) {
    for (std::size_t i = 0; i < m.nodes.size(); ++i)
        if (m.nodes[i].name == name) return static_cast<std::int32_t>(i);
    return -1;
}

} // namespace

TEST_CASE("assets fbx: ASCII cube with a Lambert material, unit conversion and import scale") {
    ImportResult r;
    auto m = import_fbx(kDir / "maya_cube_7500_ascii.fbx", {}, kDir, &r);
    REQUIRE(m.ok());
    REQUIRE(m->nodes.size() == 1);
    CHECK(m->nodes[0].name == "pCube1");
    REQUIRE(m->meshes.size() == 1);
    CHECK(m->meshes[0].data.vertices.size() == 24); // hard-edged cube: 6 faces x 4 corners after de-duplication
    CHECK(m->meshes[0].data.indices.size() == 36);
    REQUIRE(m->materials.size() == 1);
    CHECK(m->materials[0].name == "lambert1");
    CHECK(m->materials[0].base_color.r == doctest::Approx(0.4f).epsilon(0.01));
    // Converted to metres from the file's unit scale; a cube stays a cube.
    const Bounds b = world_bounds(*m, 0);
    const float side = b.hi.x - b.lo.x;
    CHECK(side > 0.0f);
    CHECK(b.hi.y - b.lo.y == doctest::Approx(side).epsilon(0.001));
    for (const auto& v : m->meshes[0].data.vertices) CHECK(math::length(v.normal) == doctest::Approx(1.0f).epsilon(0.001));

    ModelImportSettings s;
    s.scale = 2.0f;
    auto big = import_fbx(kDir / "maya_cube_7500_ascii.fbx", s, kDir);
    REQUIRE(big.ok());
    const Bounds bb = world_bounds(*big, 0);
    CHECK(bb.hi.x - bb.lo.x == doctest::Approx(2.0f * side).epsilon(0.001));
}

TEST_CASE("assets fbx: Blender Z-up file is upright, embedded textures are cooked") {
    auto m = import_fbx(kDir / "blender_293_embedded_textures_7400_binary.fbx", {}, kDir);
    REQUIRE(m.ok());
    REQUIRE(m->meshes.size() == 1);
    CHECK(m->textures.size() == 3);
    REQUIRE(m->materials.size() == 1);
    const ModelMaterial& mat = m->materials[0];
    REQUIRE(mat.base_color_texture >= 0);
    const CookedTexture& t = m->textures[static_cast<std::size_t>(mat.base_color_texture)];
    CHECK(t.srgb);
    REQUIRE_FALSE(t.mips.empty());
    CHECK(t.mips[0].width > 0);
    for (const auto& tex : m->textures) CHECK_FALSE(tex.mips.empty());
    // The default Blender cube (2 m) is centred on the origin in Y-up space.
    const Bounds b = world_bounds(*m, 0);
    CHECK(b.hi.y - b.lo.y == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(b.hi.y + b.lo.y == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("assets fbx: skin bind pose matches the mesh and unweighted vertices stay on the mesh node") {
    ImportResult r;
    auto m = import_fbx(kDir / "blender_293_half_skinned_7400_binary.fbx", {}, kDir, &r);
    REQUIRE(m.ok());
    const std::int32_t plane = find_node(*m, "Plane");
    const std::int32_t bone = find_node(*m, "Bone");
    REQUIRE(plane >= 0);
    REQUIRE(bone >= 0);
    REQUIRE(m->skins.size() == 1);
    const ModelSkin& skin = m->skins[0];
    CHECK(m->nodes[static_cast<std::size_t>(plane)].skin == 0);
    // Bones of the deformer plus the mesh node for the vertices without weights.
    REQUIRE(skin.joints.size() >= 2);
    CHECK(skin.joints.back() == static_cast<std::uint32_t>(plane));
    const ModelMesh& mesh = m->meshes[m->nodes[static_cast<std::size_t>(plane)].meshes[0]];
    REQUIRE(mesh.data.skin.size() == mesh.data.vertices.size());
    // The file's current pose lifts the bone 1 m: skinning with the node transforms must give
    // Blender's own posed export (blender_293_half_skinned.obj, Y up), the unweighted half stays.
    auto ref = import_fbx(kDir / "blender_293_half_skinned.obj", {}, kDir);
    REQUIRE(ref.ok());
    REQUIRE(ref->meshes.size() == 1);
    const auto near_ref = [&](math::Vec3 p) {
        for (const auto& v : ref->meshes[0].data.vertices)
            if (math::length(v.position - p) < 1e-3f) return true;
        return false;
    };
    const math::Mat4 mesh_world = world(*m, plane);
    std::size_t rigid = 0, bound = 0;
    for (std::size_t i = 0; i < mesh.data.vertices.size(); ++i) {
        const render::SkinVertex& sv = mesh.data.skin[i];
        const math::Vec3 p = mesh.data.vertices[i].position;
        const math::Vec3 expected = mesh_world.transform_point(p);
        math::Vec3 skinned{};
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            if (sv.weights[k] <= 0.0f) continue;
            const math::Mat4 palette = world(*m, static_cast<std::int32_t>(skin.joints[sv.joints[k]])) * skin.inverse_bind[sv.joints[k]];
            skinned = skinned + palette.transform_point(p) * sv.weights[k];
            wsum += sv.weights[k];
        }
        CHECK(wsum == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(near_ref(skinned));
        if (sv.joints[0] == skin.joints.size() - 1) {
            CHECK(math::length(skinned - expected) < 1e-3f); // unweighted: exactly the static mesh
            ++rigid;
        } else {
            CHECK(math::length(skinned - expected) > 0.5f); // follows the posed bone
            ++bound;
        }
    }
    CHECK(rigid > 0);
    CHECK(bound > 0);
}

TEST_CASE("assets fbx: blend shapes become morph targets with an animated weights channel") {
    auto m = import_fbx(kDir / "maya_blend_shape_cube_7700_ascii.fbx", {}, kDir);
    REQUIRE(m.ok());
    REQUIRE(m->meshes.size() == 1);
    const auto& data = m->meshes[0].data;
    REQUIRE(data.morphs.size() == 2);
    CHECK(data.morph_weights.size() == 2);
    for (const auto& t : data.morphs) {
        CHECK_FALSE(t.vertices.empty());
        CHECK(t.vertices.size() == t.positions.size());
        CHECK(std::is_sorted(t.vertices.begin(), t.vertices.end()));
        for (const std::uint32_t v : t.vertices) CHECK(v < data.vertices.size());
    }
    REQUIRE(m->animations.size() == 1);
    const animation::Clip& clip = m->animations[0];
    CHECK(clip.duration > 0.0f);
    const animation::Channel* weights = nullptr;
    for (const auto& ch : clip.channels)
        if (ch.path == animation::Path::Weights) weights = &ch;
    REQUIRE(weights);
    CHECK(weights->width == 2);
    REQUIRE(weights->values.size() == weights->times.size() * 2);
    float lo = 1e9f, hi = -1e9f;
    for (const float w : weights->values) {
        lo = std::min(lo, w);
        hi = std::max(hi, w);
    }
    CHECK(lo >= -0.001f);
    CHECK(hi <= 1.001f);
    CHECK(hi - lo > 0.5f); // DeformPercent 0..100 normalised to 0..1 and actually animated
}

TEST_CASE("assets fbx: lights and cameras look along -Z") {
    ImportResult r;
    auto m = import_fbx(kDir / "maya_camera_light_axes_y_up_7700_ascii.fbx", {}, kDir, &r);
    REQUIRE(m.ok());
    CHECK(m->meshes.empty());
    REQUIRE(m->lights.size() == 1);
    REQUIRE(m->cameras.size() == 1);
    CHECK(m->lights[0].type == ModelLight::Type::Directional);
    CHECK(m->lights[0].name == "forwardLight");
    for (const auto& w : r.warnings) CHECK(w.find("not -Z") == std::string::npos);
    const ModelCamera& cam = m->cameras[0];
    CHECK_FALSE(cam.orthographic);
    CHECK(cam.yfov > 0.1f);
    CHECK(cam.yfov < 3.0f);
    CHECK(cam.znear > 0.0f);
    CHECK(cam.zfar > cam.znear);
    CHECK(cam.aspect == doctest::Approx(16.0f / 9.0f).epsilon(0.01));
    // Spot cones arrive as half-angles in radians; a Maya point light keeps its colour.
    auto lit = import_fbx(kDir / "maya_anim_light_7500_ascii.fbx", {}, kDir);
    REQUIRE(lit.ok());
    REQUIRE(lit->lights.size() == 1);
    CHECK(lit->lights[0].type == ModelLight::Type::Point);
    CHECK(lit->lights[0].intensity > 0.0f);
    CHECK(lit->lights[0].color.b > lit->lights[0].color.r);

    ModelImportSettings off;
    off.import_lights = false;
    off.import_cameras = false;
    CHECK_FALSE(import_fbx(kDir / "maya_camera_light_axes_y_up_7700_ascii.fbx", off, kDir).ok()); // nothing left
}

TEST_CASE("assets fbx: OBJ with MTL and a texture, sandbox and memory import") {
    TempDir dir("victus_obj");
    render::Image img;
    img.width = img.height = 4;
    img.pixels.assign(4 * 4 * 4, 0);
    for (std::size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i] = 255;
        img.pixels[i + 3] = 255;
    }
    REQUIRE(render::save_png(dir.path / "assets" / "red.png", img).ok());
    write_text(dir.path / "assets" / "quad.mtl", "newmtl Red\nKd 0.8 0.1 0.1\nmap_Kd red.png\nnewmtl Blue\nKd 0.1 0.1 0.9\n");
    const std::string geometry = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 2 0 0\nv 2 1 0\n"
                                 "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n";
    write_text(dir.path / "assets" / "quad.obj", "mtllib quad.mtl\n" + geometry +
                                                     "o Quad\nusemtl Red\nf 1/1/1 2/2/1 3/3/1 4/4/1\nusemtl Blue\nf 2/1/1 5/2/1 6/3/1 3/4/1\n");
    ImportResult r;
    auto m = import_fbx(dir.path / "assets" / "quad.obj", {}, dir.path / "assets", &r);
    REQUIRE(m.ok());
    REQUIRE(m->meshes.size() == 2); // split per material
    REQUIRE(m->materials.size() == 2);
    const ModelMaterial& red = m->materials[m->meshes[0].material];
    CHECK(red.name == "Red");
    CHECK(red.base_color.r == doctest::Approx(0.8f).epsilon(0.01));
    REQUIRE(red.base_color_texture >= 0);
    CHECK(m->textures[static_cast<std::size_t>(red.base_color_texture)].mips[0].pixel(0, 0) == 0xff0000ffu);
    CHECK(m->materials[m->meshes[1].material].name == "Blue");
    for (const auto& mesh : m->meshes) {
        CHECK(mesh.data.vertices.size() == 4);
        CHECK(mesh.data.indices.size() == 6);
    }
    // OBJ v = 0 is the bottom of the image; VICTUS (like glTF) has v = 0 at the top.
    bool found = false;
    for (const auto& v : m->meshes[0].data.vertices)
        if (v.position.x == 0.0f && v.position.y == 0.0f) {
            CHECK(v.uv.y == doctest::Approx(1.0f));
            found = true;
        }
    CHECK(found);
    std::size_t deps = 0;
    for (const auto& d : r.dependencies) deps += d.filename() == "quad.mtl" || d.filename() == "red.png";
    CHECK(deps == 2);

    // A material library outside the asset directory is never read.
    write_text(dir.path / "secret.mtl", "newmtl Red\nKd 0 1 0\n");
    write_text(dir.path / "assets" / "escape.obj", "mtllib ../secret.mtl\n" + geometry + "usemtl Red\nf 1/1/1 2/2/1 3/3/1 4/4/1\n");
    ImportResult er;
    auto esc = import_fbx(dir.path / "assets" / "escape.obj", {}, dir.path / "assets", &er);
    REQUIRE(esc.ok());
    for (const auto& d : er.dependencies) CHECK(d.filename() != "secret.mtl");
    CHECK(esc->materials[0].base_color.r > 0.5f); // not the green outside material

    // From memory: geometry only, no external reads.
    const std::string text = "mtllib quad.mtl\n" + geometry + "f 1/1/1 2/2/1 3/3/1\n";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    ImportResult mr;
    auto mem = import_fbx_memory(bytes, "tri.obj", {}, &mr);
    REQUIRE(mem.ok());
    CHECK(mem->meshes.size() == 1);
    CHECK(mr.dependencies.empty());
    CHECK_FALSE(import_fbx_memory(std::vector<std::uint8_t>{1, 2, 3, 4}, "junk.fbx", {}).ok());
}

TEST_CASE("assets fbx: the asset database imports .fbx and .obj as models") {
    CHECK(kind_for_extension(".fbx") == AssetKind::Model);
    CHECK(kind_for_extension(".FBX") == AssetKind::Model);
    CHECK(kind_for_extension(".obj") == AssetKind::Model);
    TempDir project("victus_fbx_project");
    std::filesystem::create_directories(project.path / "assets" / "models");
    std::filesystem::copy_file(kDir / "maya_blend_shape_cube_7700_ascii.fbx", project.path / "assets" / "models" / "cube.fbx");
    auto opened = AssetDatabase::open(project.path);
    REQUIRE(opened.ok());
    AssetDatabase& db = **opened;
    const AssetRecord* rec = db.find_path("models/cube.fbx");
    REQUIRE(rec);
    CHECK(rec->kind == AssetKind::Model);
    const Uuid id = rec->uuid;
    jobs::JobSystem js(2);
    const ImportReport ir = db.import_all(&js);
    CHECK(ir.imported == 1);
    CHECK(ir.failed == 0);
    auto model = db.load_model(id);
    REQUIRE(model.ok());
    CHECK(model->meshes.size() == 1);
    CHECK(model->meshes[0].data.morphs.size() == 2);
    CHECK(model->animations.size() == 1);
}

TEST_CASE("assets: skin weight warnings describe dropped, merged and missing influences") {
    victus::render::SkinWeightStats s;
    CHECK(victus::assets::skin_weight_warnings("Body", s).empty());
    s.vertices = 10;
    s.over_limit = 3;
    s.max_dropped_weight = 0.08f;
    s.duplicate_joints = 1;
    s.unweighted = 2;
    const auto w = victus::assets::skin_weight_warnings("Body", s);
    REQUIRE(w.size() == 3);
    CHECK(w[0].find("2 of 10") != std::string::npos);
    CHECK(w[1].find("3 vertices have more than 4") != std::string::npos);
    CHECK(w[1].find("0.080") != std::string::npos);
    CHECK(w[2].find("merged") != std::string::npos);
}
