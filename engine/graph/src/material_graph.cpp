// SPDX-License-Identifier: Apache-2.0
// Material graphs: node library and the compiler to surface-shader GLSL (ADR-0062).
#include "victus/graph/material_graph.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace victus::graph {

using json::Value;

namespace {

using T = PinType;

PinDesc pin(std::string name, PinType type, Value def = {}) { return {std::move(name), type, std::move(def)}; }
Value arr(std::initializer_list<double> v) {
    json::Array a;
    for (double d : v) a.emplace_back(d);
    return Value(std::move(a));
}
// A default that is a GLSL expression rather than a literal (shown as text in the editor).
Value expr(std::string_view e) { return Value("=" + std::string(e)); }

// Helper functions emitted once before the surface function when a node needs them.
constexpr std::string_view kNoiseHelpers = R"(float victus_mg_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float victus_mg_noise(vec2 p) {
    vec2 c = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = victus_mg_hash(c);
    float b = victus_mg_hash(c + vec2(1.0, 0.0));
    float d = victus_mg_hash(c + vec2(0.0, 1.0));
    float e = victus_mg_hash(c + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(d, e, u.x), u.y);
}
)";

std::vector<NodeDesc> build_library() {
    std::vector<NodeDesc> L;
    auto node = [&](std::string type, std::string category, std::string title, std::vector<PinDesc> ins, std::vector<PinDesc> outs,
                    std::vector<std::string> code, std::string tip) {
        L.push_back({std::move(type), std::move(category), std::move(title), std::move(tip), NodeKind::Pure, std::move(ins),
                     std::move(outs), {}, {}, std::move(code)});
    };

    // Output.
    node(std::string(kMaterialOutputNode), "Output", "Surface Output",
         {pin("base_color", T::Vector, expr("s.base_color")), pin("alpha", T::Number, expr("s.alpha")),
          pin("normal", T::Vector, expr("s.normal")), pin("metallic", T::Number, expr("s.metallic")),
          pin("roughness", T::Number, expr("s.roughness")), pin("emissive", T::Vector, expr("s.emissive")),
          pin("occlusion", T::Number, expr("s.occlusion")), pin("unlit", T::Bool, Value(false))},
         {}, {},
         "The shaded surface. Linked inputs replace the material's values; unlinked ones keep them. Normal is a world-space "
         "direction (normalized for you).");

    // Inputs.
    node("mat.surface", "Input", "Material Surface", {},
         {pin("base_color", T::Vector), pin("alpha", T::Number), pin("normal", T::Vector), pin("metallic", T::Number),
          pin("roughness", T::Number), pin("emissive", T::Vector), pin("occlusion", T::Number)},
         {"s.base_color", "s.alpha", "s.normal", "s.metallic", "s.roughness", "s.emissive", "s.occlusion"},
         "The material's own values (factors and textures applied) before the graph changes them.");
    node("mat.uv", "Input", "UV", {}, {pin("uv", T::Vec2)}, {"i.uv"}, "Texture coordinates with the material's tiling and offset.");
    node("mat.mesh_uv", "Input", "Mesh UV", {}, {pin("uv", T::Vec2)}, {"i.mesh_uv"}, "The mesh's own texture coordinates.");
    node("mat.world_position", "Input", "World Position", {}, {pin("position", T::Vector)}, {"i.world_position"},
         "The pixel's position in world space.");
    node("mat.view_dir", "Input", "View Direction", {}, {pin("direction", T::Vector)}, {"i.view_dir"},
         "Unit vector from the surface towards the camera.");
    node("mat.vertex_normal", "Input", "Vertex Normal", {}, {pin("normal", T::Vector)}, {"i.vertex_normal"},
         "Interpolated geometric normal in world space (before normal maps).");
    node("mat.vertex_color", "Input", "Vertex Color", {}, {pin("color", T::Vec4), pin("rgb", T::Vector), pin("alpha", T::Number)},
         {"i.vertex_color", "i.vertex_color.rgb", "i.vertex_color.a"}, "The mesh's interpolated vertex colour.");
    node("mat.time", "Input", "Time", {}, {pin("seconds", T::Number)}, {"i.time"}, "Seconds since the game started.");
    for (int p = 0; p < 4; ++p)
        node(std::format("mat.param{}", p), "Input", std::format("Parameter {}", p), {},
             {pin("value", T::Vec4), pin("rgb", T::Vector), pin("x", T::Number), pin("y", T::Number), pin("z", T::Number),
              pin("w", T::Number)},
             {std::format("i.params[{}]", p), std::format("i.params[{}].rgb", p), std::format("i.params[{}].x", p),
              std::format("i.params[{}].y", p), std::format("i.params[{}].z", p), std::format("i.params[{}].w", p)},
             std::format("The material's shader parameter {} (set per material in the Material Editor).", p));

    for (const auto& [slot, title] : {std::pair{"base_color", "Base Color"}, std::pair{"metal_rough", "Metal/Rough"},
                                      std::pair{"normal_map", "Normal Map"}, std::pair{"emissive", "Emissive"}}) {
        const std::string s = slot;
        node("mat.texture_" + s, "Texture", std::format("{} Texture", title), {pin("uv", T::Vec2, expr("i.uv"))},
             {pin("color", T::Vec4), pin("rgb", T::Vector), pin("alpha", T::Number)},
             {"texture(u_" + s + ", {uv})", "texture(u_" + s + ", {uv}).rgb", "texture(u_" + s + ", {uv}).a"},
             std::format("Samples the material's {} texture (white when unset) at any coordinates.", title));
    }

    // Constants.
    node("mat.float", "Constant", "Float", {pin("value", T::Number, Value(1.0))}, {pin("value", T::Number)}, {"{value}"}, "A number.");
    node("mat.vector2", "Constant", "Vector 2", {pin("value", T::Vec2, arr({0, 0}))}, {pin("value", T::Vec2)}, {"{value}"},
         "A two-component vector.");
    node("mat.vector3", "Constant", "Vector 3", {pin("value", T::Vector, arr({0, 0, 0}))}, {pin("value", T::Vector)}, {"{value}"},
         "A three-component vector.");
    node("mat.color", "Constant", "Color", {pin("color", T::Vec4, arr({1, 1, 1, 1}))},
         {pin("color", T::Vec4), pin("rgb", T::Vector), pin("alpha", T::Number)}, {"{color}", "{color}.rgb", "{color}.a"},
         "A linear colour with alpha.");

    // Math (generic: the widest connected type wins).
    auto binary = [&](std::string type, std::string title, std::string code, double b_default, std::string tip) {
        node("mat." + type, "Math", std::move(title), {pin("a", T::Any, Value(0.0)), pin("b", T::Any, Value(b_default))},
             {pin("result", T::Any)}, {std::move(code)}, std::move(tip));
    };
    binary("add", "Add", "({a} + {b})", 0.0, "a + b");
    binary("subtract", "Subtract", "({a} - {b})", 0.0, "a - b");
    binary("multiply", "Multiply", "({a} * {b})", 1.0, "a * b, per component");
    binary("divide", "Divide", "({a} / {b})", 1.0, "a / b, per component");
    binary("min", "Min", "min({a}, {b})", 0.0, "The smaller of a and b, per component.");
    binary("max", "Max", "max({a}, {b})", 0.0, "The larger of a and b, per component.");
    binary("power", "Power", "pow(max({a}, {T}(0.0)), {b})", 2.0, "a raised to b (a is clamped to zero or more).");
    binary("step", "Step", "step({a}, {b})", 0.5, "0 where b < a (the edge), otherwise 1.");
    auto unary = [&](std::string type, std::string title, std::string code, std::string tip) {
        node("mat." + type, "Math", std::move(title), {pin("x", T::Any, Value(0.0))}, {pin("result", T::Any)}, {std::move(code)},
             std::move(tip));
    };
    unary("one_minus", "One Minus", "({T}(1.0) - {x})", "1 - x");
    unary("negate", "Negate", "(-{x})", "-x");
    unary("abs", "Absolute", "abs({x})", "|x|");
    unary("saturate", "Saturate", "clamp({x}, 0.0, 1.0)", "x clamped to 0..1");
    unary("fract", "Fraction", "fract({x})", "The fractional part of x.");
    unary("floor", "Floor", "floor({x})", "x rounded down.");
    unary("sqrt", "Square Root", "sqrt(max({x}, {T}(0.0)))", "Square root (negative inputs give 0).");
    unary("sin", "Sine", "sin({x})", "sin(x), x in radians.");
    unary("cos", "Cosine", "cos({x})", "cos(x), x in radians.");
    node("mat.lerp", "Math", "Lerp", {pin("a", T::Any, Value(0.0)), pin("b", T::Any, Value(1.0)), pin("t", T::Number, Value(0.5))},
         {pin("result", T::Any)}, {"mix({a}, {b}, {t})"}, "Blends from a (t = 0) to b (t = 1).");
    node("mat.clamp", "Math", "Clamp", {pin("x", T::Any, Value(0.0)), pin("min", T::Number, Value(0.0)), pin("max", T::Number, Value(1.0))},
         {pin("result", T::Any)}, {"clamp({x}, {min}, max({min}, {max}))"}, "x limited to min..max.");
    node("mat.smoothstep", "Math", "Smoothstep",
         {pin("edge0", T::Number, Value(0.0)), pin("edge1", T::Number, Value(1.0)), pin("x", T::Any, Value(0.5))}, {pin("result", T::Any)},
         {"smoothstep({edge0}, {edge1}, {x})"}, "Smooth 0..1 ramp of x between edge0 and edge1.");
    node("mat.remap", "Math", "Remap",
         {pin("x", T::Any, Value(0.5)), pin("from_min", T::Number, Value(0.0)), pin("from_max", T::Number, Value(1.0)),
          pin("to_min", T::Number, Value(0.0)), pin("to_max", T::Number, Value(1.0))},
         {pin("result", T::Any)},
         {"({to_min} + ({x} - {from_min}) * (({to_max} - {to_min}) / (abs({from_max} - {from_min}) < 1e-6 ? 1e-6 : {from_max} - "
          "{from_min})))"},
         "Maps x from one range to another (not clamped).");

    // Vector.
    node("mat.dot", "Vector", "Dot", {pin("a", T::Any, arr({0, 0, 1})), pin("b", T::Any, arr({0, 0, 1}))}, {pin("result", T::Number)},
         {"dot({a}, {b})"}, "Dot product.");
    node("mat.cross", "Vector", "Cross", {pin("a", T::Vector, arr({1, 0, 0})), pin("b", T::Vector, arr({0, 1, 0}))},
         {pin("result", T::Vector)}, {"cross({a}, {b})"}, "Cross product.");
    node("mat.normalize", "Vector", "Normalize", {pin("v", T::Any, arr({0, 0, 1}))}, {pin("result", T::Any)},
         {"({v} / max(length({v}), 1e-6))"}, "The vector scaled to length 1.");
    node("mat.length", "Vector", "Length", {pin("v", T::Any, arr({0, 0, 0}))}, {pin("length", T::Number)}, {"length({v})"},
         "The vector's length.");
    node("mat.distance", "Vector", "Distance", {pin("a", T::Any, arr({0, 0, 0})), pin("b", T::Any, arr({0, 0, 0}))},
         {pin("distance", T::Number)}, {"distance({a}, {b})"}, "Distance between two points.");
    node("mat.split", "Vector", "Split", {pin("v", T::Vec4, arr({0, 0, 0, 0}))},
         {pin("x", T::Number), pin("y", T::Number), pin("z", T::Number), pin("w", T::Number)}, {"{v}.x", "{v}.y", "{v}.z", "{v}.w"},
         "The components of a vector.");
    node("mat.combine", "Vector", "Combine",
         {pin("x", T::Number, Value(0.0)), pin("y", T::Number, Value(0.0)), pin("z", T::Number, Value(0.0)), pin("w", T::Number, Value(1.0))},
         {pin("xy", T::Vec2), pin("xyz", T::Vector), pin("xyzw", T::Vec4)},
         {"vec2({x}, {y})", "vec3({x}, {y}, {z})", "vec4({x}, {y}, {z}, {w})"}, "Builds a vector from numbers.");

    // UV and effects.
    node("mat.tiling", "UV", "Tiling and Offset", {pin("uv", T::Vec2, expr("i.uv")), pin("tiling", T::Vec2, arr({1, 1})), pin("offset", T::Vec2, arr({0, 0}))},
         {pin("uv", T::Vec2)}, {"({uv} * {tiling} + {offset})"}, "Scales then shifts texture coordinates.");
    node("mat.panner", "UV", "Panner", {pin("uv", T::Vec2, expr("i.uv")), pin("speed", T::Vec2, arr({0.1, 0}))}, {pin("uv", T::Vec2)},
         {"({uv} + {speed} * i.time)"}, "Scrolls texture coordinates over time.");
    node("mat.noise", "Effects", "Value Noise", {pin("uv", T::Vec2, expr("i.uv")), pin("scale", T::Number, Value(8.0))},
         {pin("value", T::Number)}, {"victus_mg_noise({uv} * {scale})"}, "Smooth 0..1 noise.");
    node("mat.checker", "Effects", "Checker", {pin("uv", T::Vec2, expr("i.uv")), pin("scale", T::Number, Value(8.0))},
         {pin("value", T::Number)}, {"mod(floor({uv}.x * {scale}) + floor({uv}.y * {scale}), 2.0)"}, "Alternating 0/1 squares.");
    node("mat.fresnel", "Effects", "Fresnel", {pin("normal", T::Vector, expr("s.normal")), pin("power", T::Number, Value(3.0))},
         {pin("value", T::Number)}, {"pow(1.0 - clamp(dot({normal}, i.view_dir), 0.0, 1.0), max({power}, 0.0))"},
         "Rim factor: 0 facing the camera, 1 at grazing angles.");
    node("mat.sine_wave", "Effects", "Sine Wave",
         {pin("speed", T::Number, Value(1.0)), pin("min", T::Number, Value(0.0)), pin("max", T::Number, Value(1.0))},
         {pin("value", T::Number)}, {"mix({min}, {max}, 0.5 + 0.5 * sin(i.time * {speed} * 6.2831853))"},
         "Oscillates between min and max, speed cycles per second.");
    return L;
}

const std::vector<NodeDesc>& library() {
    static const std::vector<NodeDesc> lib = build_library();
    return lib;
}

const PinDesc* find_pin(const std::vector<PinDesc>& pins, std::string_view name) {
    for (const auto& p : pins)
        if (p.name == name) return &p;
    return nullptr;
}

// GLSL widths: 1 float, 2..4 vecN; 0 generic/unknown.
int width_of(PinType t) {
    switch (t) {
    case T::Number: return 1;
    case T::Vec2: return 2;
    case T::Vector: return 3;
    case T::Vec4: return 4;
    default: return 0;
    }
}
std::string type_name(int w) { return w <= 1 ? "float" : std::format("vec{}", w); }

std::string float_literal(double d) {
    if (!std::isfinite(d)) d = 0.0;
    std::string s = std::format("{}", d);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return d < 0 ? "(" + s + ")" : s;
}

// Converts `e` of width `from` to width `to`.
std::string convert(const std::string& e, int from, int to) {
    if (from == to || to == 0) return e;
    if (from == 1) return std::format("{}({})", type_name(to), e);
    if (to == 1) return std::format("({}).x", e);
    if (to < from) return std::format("({}).{}", e, std::string_view("xyzw").substr(0, static_cast<std::size_t>(to)));
    if (from == 2 && to == 3) return std::format("vec3({}, 0.0)", e);
    if (from == 2 && to == 4) return std::format("vec4({}, 0.0, 1.0)", e);
    return std::format("vec4({}, 1.0)", e); // 3 -> 4
}

class Compiler {
public:
    Compiler(const ScriptGraph& g, std::string_view name) : g_(g), name_(name) {}

    CompiledMaterialGraph run() {
        index();
        check_links();
        if (!has_errors()) emit();
        CompiledMaterialGraph c;
        c.diagnostics = std::move(diags_);
        if (c.ok()) c.source = std::move(text_);
        return c;
    }

private:
    struct Out {
        std::uint32_t node;
        std::string pin;
    };
    const ScriptGraph& g_;
    std::string name_;
    std::vector<GraphDiagnostic> diags_;
    std::map<std::uint32_t, const GraphNode*> nodes_;
    std::map<std::uint32_t, const NodeDesc*> descs_;
    std::map<std::pair<std::uint32_t, std::string>, Out> data_in_;
    std::set<std::pair<std::uint32_t, std::string>> used_outputs_;
    std::map<std::uint32_t, int> generic_; // resolved generic width per node
    std::set<std::uint32_t> resolving_, emitted_, visiting_;
    struct Local {
        std::uint32_t node;
        std::string pin;
        std::string line;
    };
    std::vector<Local> body_;
    std::set<std::string_view> helpers_;
    std::string text_;
    std::uint32_t output_{0};

    void error(std::uint32_t node, std::string pin_name, std::string msg) {
        diags_.push_back({GraphDiagnostic::Severity::Error, node, std::move(pin_name), std::move(msg)});
    }
    void warn(std::uint32_t node, std::string msg) { diags_.push_back({GraphDiagnostic::Severity::Warning, node, {}, std::move(msg)}); }
    bool has_errors() const {
        return std::ranges::any_of(diags_, [](const GraphDiagnostic& d) { return d.severity == GraphDiagnostic::Severity::Error; });
    }

    void index() {
        if (g_.domain != GraphDomain::Material) error(0, {}, "not a material graph (its domain must be \"material\")");
        if (!g_.variables.empty()) error(0, {}, "material graphs have no variables; use the Parameter nodes");
        for (const auto& n : g_.nodes) {
            if (n.id == 0) error(0, {}, "node with id 0");
            if (!nodes_.emplace(n.id, &n).second) error(n.id, {}, "duplicate node id");
            const NodeDesc* d = find_node_desc(n.type, GraphDomain::Material);
            if (!d) {
                error(n.id, {}, std::format("unknown material node type '{}'", n.type));
                continue;
            }
            descs_[n.id] = d;
            if (d->type == kMaterialOutputNode) {
                if (output_) error(n.id, {}, "a material graph has exactly one Surface Output");
                else output_ = n.id;
            }
        }
        if (!output_) error(0, {}, "add a Surface Output node");
    }

    void check_links() {
        for (const auto& l : g_.links) {
            const auto a = nodes_.find(l.from_node), b = nodes_.find(l.to_node);
            if (a == nodes_.end() || b == nodes_.end()) {
                error(a == nodes_.end() ? 0 : l.from_node, {}, std::format("link to missing node {}", a == nodes_.end() ? l.from_node : l.to_node));
                continue;
            }
            if (!descs_.contains(l.from_node) || !descs_.contains(l.to_node)) continue;
            const PinDesc* o = find_pin(descs_[l.from_node]->outputs, l.from_pin);
            const PinDesc* i = find_pin(descs_[l.to_node]->inputs, l.to_pin);
            if (!o) {
                error(l.from_node, l.from_pin, "link from a pin this node does not have");
                continue;
            }
            if (!i) {
                error(l.to_node, l.to_pin, "link into a pin this node does not have");
                continue;
            }
            if (!pin_types_compatible(o->type, i->type, GraphDomain::Material)) {
                error(l.to_node, l.to_pin, std::format("cannot connect {} output '{}' to {} input", pin_type_name(o->type), l.from_pin,
                                                       pin_type_name(i->type)));
                continue;
            }
            if (!data_in_.emplace(std::pair{l.to_node, l.to_pin}, Out{l.from_node, l.from_pin}).second)
                error(l.to_node, l.to_pin, "an input can only have one link");
        }
    }

    // Width of an unlinked input's literal (generic pins take it from the value).
    static int literal_width(const PinDesc& p, const Value& v) {
        if (const int w = width_of(p.type)) return w;
        if (v.is_array()) return std::clamp(static_cast<int>(v.size()), 1, 4);
        return 1;
    }

    const Value& literal_of(std::uint32_t id, const PinDesc& p) const {
        const Value* lit = nodes_.at(id)->inputs.find(p.name);
        return lit ? *lit : p.default_value;
    }

    std::string literal_expr(const PinDesc& p, const Value& v, int w) {
        if (v.is_string() && v.as_string().starts_with("=")) return v.as_string().substr(1);
        if (p.type == T::Bool) return v.as_bool(false) ? "true" : "false";
        if (w <= 1) return float_literal(v.is_array() && v.size() > 0 ? v[0].as_number() : v.as_number());
        std::string out = type_name(w) + "(";
        for (int c = 0; c < w; ++c) {
            double d = 0.0;
            if (v.is_array()) d = c < static_cast<int>(v.size()) ? v[static_cast<std::size_t>(c)].as_number() : (c == 3 ? 1.0 : 0.0);
            else d = v.as_number();
            out += (c ? ", " : "") + float_literal(d);
        }
        return out + ")";
    }

    int output_width(std::uint32_t id, const std::string& pin_name) {
        const PinDesc* p = find_pin(descs_.at(id)->outputs, pin_name);
        if (!p) return 1;
        if (const int w = width_of(p->type)) return w;
        return generic_width(id);
    }

    int input_width(std::uint32_t id, const PinDesc& p) {
        if (const auto it = data_in_.find({id, p.name}); it != data_in_.end()) return output_width(it->second.node, it->second.pin);
        return literal_width(p, literal_of(id, p));
    }

    int generic_width(std::uint32_t id) {
        if (const auto it = generic_.find(id); it != generic_.end()) return it->second;
        if (!resolving_.insert(id).second) return 1; // cycle: reported by emit
        int w = 1;
        for (const auto& p : descs_.at(id)->inputs)
            if (p.type == T::Any) w = std::max(w, input_width(id, p));
        resolving_.erase(id);
        generic_[id] = w;
        return w;
    }

    // Expression for input `p` of `id`, converted to `want` (0: the input's own width).
    std::string input_expr(std::uint32_t id, const PinDesc& p, int want) {
        if (const auto it = data_in_.find({id, p.name}); it != data_in_.end()) {
            const Out src = it->second;
            if (!visit(src.node)) return "0.0";
            used_outputs_.insert({src.node, src.pin});
            return convert(std::format("n{}_{}", src.node, src.pin), output_width(src.node, src.pin), want);
        }
        const Value& v = literal_of(id, p);
        const int w = literal_width(p, v);
        return convert(literal_expr(p, v, w), w, want);
    }

    std::string expand(std::uint32_t id, std::string_view tmpl) {
        const NodeDesc& d = *descs_.at(id);
        const int g = generic_width(id);
        std::string out;
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] != '{') {
                out += tmpl[i];
                continue;
            }
            const std::size_t end = tmpl.find('}', i);
            const std::string_view key = tmpl.substr(i + 1, end - i - 1);
            i = end;
            if (key == "T") {
                out += type_name(g);
                continue;
            }
            const PinDesc* p = find_pin(d.inputs, key);
            if (!p) continue;
            const int want = p->type == T::Any ? g : width_of(p->type);
            std::string e = input_expr(id, *p, want);
            out += e;
        }
        return out;
    }

    // Emits the locals of `id` (after its dependencies). False on a cycle.
    bool visit(std::uint32_t id) {
        if (emitted_.contains(id)) return true;
        if (!visiting_.insert(id).second) {
            error(id, {}, "this node feeds back into itself (cycles are not allowed)");
            return false;
        }
        const NodeDesc& d = *descs_.at(id);
        if (d.type == "mat.noise") helpers_.insert(kNoiseHelpers);
        std::vector<Local> lines;
        for (std::size_t o = 0; o < d.outputs.size() && o < d.output_code.size(); ++o) {
            const std::string& pin_name = d.outputs[o].name;
            const std::string value = expand(id, d.output_code[o]);
            lines.push_back({id, pin_name, std::format("    {} n{}_{} = {};", type_name(output_width(id, pin_name)), id, pin_name, value)});
        }
        visiting_.erase(id);
        emitted_.insert(id);
        for (auto& l : lines) body_.push_back(std::move(l));
        return true;
    }

    void emit() {
        const NodeDesc& out = *descs_.at(output_);
        std::vector<std::string> assigns;
        for (const auto& p : out.inputs) {
            if (p.type == T::Bool) {
                if (literal_of(output_, p).as_bool(false)) assigns.push_back(std::format("    s.{} = true;", p.name));
                continue;
            }
            // Unlinked inputs keep the material's value unless given a constant.
            if (!data_in_.contains({output_, p.name})) {
                const Value& v = literal_of(output_, p);
                if (v.is_string() && v.as_string().starts_with("=")) continue;
            }
            std::string e = input_expr(output_, p, width_of(p.type));
            if (p.name == "normal") e = std::format("normalize({})", e);
            assigns.push_back(std::format("    s.{} = {};", p.name, e));
        }
        emitted_.insert(output_);
        if (has_errors()) return;
        for (const auto& [id, d] : descs_)
            if (!emitted_.contains(id)) warn(id, "not connected to the Surface Output; it has no effect");

        text_ = std::format("// Generated from {} by the VICTUS material graph compiler (ADR-0062). Do not edit.\n",
                            name_.empty() ? std::string("a material graph") : name_);
        for (std::string_view h : helpers_) text_ += h;
        text_ += "void surface(SurfaceInput i, inout Surface s) {\n";
        // Only outputs something reads are kept (each node computes every output).
        for (const auto& l : body_)
            if (used_outputs_.contains({l.node, l.pin})) text_ += l.line + "\n";
        for (const auto& a : assigns) text_ += a + "\n";
        text_ += "}\n";
    }
};

} // namespace

std::span<const NodeDesc> material_node_library() { return library(); }

bool CompiledMaterialGraph::ok() const noexcept {
    return std::ranges::none_of(diagnostics, [](const GraphDiagnostic& d) { return d.severity == GraphDiagnostic::Severity::Error; });
}

std::string CompiledMaterialGraph::error_text() const {
    std::string out;
    for (const auto& d : diagnostics) {
        if (d.severity != GraphDiagnostic::Severity::Error) continue;
        if (!out.empty()) out += "\n";
        out += d.node ? std::format("node {}: {}", d.node, d.message) : d.message;
    }
    return out;
}

CompiledMaterialGraph compile_material_graph(const ScriptGraph& graph, std::string_view source_name) {
    return Compiler(graph, source_name).run();
}

std::vector<GraphDiagnostic> validate_material_graph(const ScriptGraph& graph) { return compile_material_graph(graph).diagnostics; }

Result<std::string> material_graph_to_glsl(std::string_view text, std::string_view name) {
    auto g = parse_graph(text);
    if (!g) return make_error(g.error().code, std::format("{}: {}", name, g.error().message));
    const CompiledMaterialGraph c = compile_material_graph(*g, name);
    if (!c.ok()) {
        std::string msg;
        for (const auto& d : c.diagnostics)
            if (d.severity == GraphDiagnostic::Severity::Error) {
                const GraphNode* n = d.node ? g->node(d.node) : nullptr;
                const NodeDesc* desc = n ? find_node_desc(n->type, GraphDomain::Material) : nullptr;
                msg += std::format("{}{}: {}{}", msg.empty() ? "" : "\n", name,
                                   d.node ? std::format("node {} ({}): ", d.node, desc ? desc->title : n ? n->type : "?") : "", d.message);
            }
        return make_error(ErrorCode::InvalidArgument, msg);
    }
    return c.source;
}

ScriptGraph starter_material_graph() {
    ScriptGraph g;
    g.domain = GraphDomain::Material;
    const auto surface = g.add_node("mat.surface", 0, 0);
    const auto color = g.add_node("mat.color", 0, 200);
    const auto mul = g.add_node("mat.multiply", 260, 60);
    const auto out = g.add_node(std::string(kMaterialOutputNode), 520, 0);
    g.node(color)->inputs.set("color", arr({1.0, 0.85, 0.7, 1.0}));
    (void)g.connect(surface, "base_color", mul, "a");
    (void)g.connect(color, "rgb", mul, "b");
    (void)g.connect(mul, "result", out, "base_color");
    return g;
}

} // namespace victus::graph
