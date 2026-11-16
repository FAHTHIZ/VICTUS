// SPDX-License-Identifier: Apache-2.0
// Test-only glTF 2.0 generator so asset tests need no third-party sample files.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace victus::test {

inline std::string base64(const std::vector<std::uint8_t>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t{in[i]} << 16) | (std::uint32_t{in[i + 1]} << 8) | std::uint32_t{in[i + 2]};
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const std::uint32_t v = std::uint32_t{in[i]} << 16;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const std::uint32_t v = (std::uint32_t{in[i]} << 16) | (std::uint32_t{in[i + 1]} << 8);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// Two-node scene: "Root" (translated +2 X) with child "Child" (scaled 2, translated +1 Y);
// both instance the same mesh: a unit quad in XZ (4 vertices, 6 uint16 indices, normals, uvs).
struct GltfFixture {
    std::vector<std::uint8_t> bin;
    json::Value doc;

    GltfFixture() {
        const float positions[] = {-0.5f, 0, -0.5f, 0.5f, 0, -0.5f, 0.5f, 0, 0.5f, -0.5f, 0, 0.5f};
        const float normals[] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
        const float uvs[] = {0, 0, 1, 0, 1, 1, 0, 1};
        const std::uint16_t indices[] = {0, 2, 1, 0, 3, 2};
        append(positions, sizeof positions);
        append(normals, sizeof normals);
        append(uvs, sizeof uvs);
        append(indices, sizeof indices);
        while (bin.size() % 4) bin.push_back(0);

        using json::Value;
        doc = Value::object();
        Value asset = Value::object();
        asset.set("version", "2.0");
        doc.set("asset", asset);
        doc.set("scene", 0);
        Value scene = Value::object();
        scene.set("nodes", Value(json::Array{0}));
        doc.set("scenes", Value(json::Array{scene}));
        Value root = Value::object();
        root.set("name", "Root");
        root.set("mesh", 0);
        root.set("translation", Value(json::Array{2.0, 0.0, 0.0}));
        root.set("children", Value(json::Array{1}));
        Value child = Value::object();
        child.set("name", "Child");
        child.set("mesh", 0);
        child.set("translation", Value(json::Array{0.0, 1.0, 0.0}));
        child.set("scale", Value(json::Array{2.0, 2.0, 2.0}));
        doc.set("nodes", Value(json::Array{root, child}));

        Value attrs = Value::object();
        attrs.set("POSITION", 0);
        attrs.set("NORMAL", 1);
        attrs.set("TEXCOORD_0", 2);
        Value prim = Value::object();
        prim.set("attributes", attrs);
        prim.set("indices", 3);
        prim.set("material", 0);
        Value mesh = Value::object();
        mesh.set("name", "Quad");
        mesh.set("primitives", Value(json::Array{prim}));
        doc.set("meshes", Value(json::Array{mesh}));

        doc.set("bufferViews", Value(json::Array{view(0, 48), view(48, 48), view(96, 32), view(128, 12)}));
        Value pos_acc = accessor(0, 5126, 4, "VEC3");
        pos_acc.set("min", Value(json::Array{-0.5, 0.0, -0.5}));
        pos_acc.set("max", Value(json::Array{0.5, 0.0, 0.5}));
        doc.set("accessors", Value(json::Array{pos_acc, accessor(1, 5126, 4, "VEC3"), accessor(2, 5126, 4, "VEC2"),
                                               accessor(3, 5123, 6, "SCALAR")}));
        Value pbr = Value::object();
        pbr.set("baseColorFactor", Value(json::Array{0.8, 0.2, 0.1, 1.0}));
        pbr.set("metallicFactor", 0.25);
        pbr.set("roughnessFactor", 0.75);
        Value mat = Value::object();
        mat.set("name", "Brick");
        mat.set("pbrMetallicRoughness", pbr);
        mat.set("doubleSided", true);
        doc.set("materials", Value(json::Array{mat}));
    }

    // Adds a base color texture from `image_uri` (file name or data URI).
    void add_texture(const std::string& image_uri) {
        using json::Value;
        Value img = Value::object();
        img.set("uri", image_uri);
        doc.set("images", Value(json::Array{img}));
        Value tex = Value::object();
        tex.set("source", 0);
        doc.set("textures", Value(json::Array{tex}));
        Value info = Value::object();
        info.set("index", 0);
        json::Value mat = doc["materials"][0];
        json::Value pbr = mat["pbrMetallicRoughness"];
        pbr.set("baseColorTexture", info);
        mat.set("pbrMetallicRoughness", pbr);
        doc.set("materials", Value(json::Array{mat}));
    }

    void set_buffer_uri(const std::string& uri) {
        json::Value buf = json::Value::object();
        buf.set("byteLength", static_cast<std::uint64_t>(bin.size()));
        if (!uri.empty()) buf.set("uri", uri);
        doc.set("buffers", json::Value(json::Array{buf}));
    }

    std::string gltf_with_data_uri() {
        set_buffer_uri("data:application/octet-stream;base64," + base64(bin));
        return json::stringify(doc, false);
    }

    std::vector<std::uint8_t> glb() {
        set_buffer_uri("");
        std::string js = json::stringify(doc, false);
        while (js.size() % 4) js.push_back(' ');
        std::vector<std::uint8_t> out;
        auto u32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        const auto total = static_cast<std::uint32_t>(12 + 8 + js.size() + 8 + bin.size());
        u32(0x46546C67);
        u32(2);
        u32(total);
        u32(static_cast<std::uint32_t>(js.size()));
        u32(0x4E4F534A);
        out.insert(out.end(), js.begin(), js.end());
        u32(static_cast<std::uint32_t>(bin.size()));
        u32(0x004E4942);
        out.insert(out.end(), bin.begin(), bin.end());
        return out;
    }

private:
    void append(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bin.insert(bin.end(), b, b + n);
    }
    static json::Value view(std::uint64_t offset, std::uint64_t length) {
        json::Value v = json::Value::object();
        v.set("buffer", 0);
        v.set("byteOffset", offset);
        v.set("byteLength", length);
        return v;
    }
    static json::Value accessor(int view_index, int component, int count, const char* type) {
        json::Value a = json::Value::object();
        a.set("bufferView", view_index);
        a.set("componentType", component);
        a.set("count", count);
        a.set("type", type);
        return a;
    }
};

} // namespace victus::test
