// SPDX-License-Identifier: Apache-2.0
// Visual scripting graphs: node library, deterministic serialization, validation and the
// compiler to Luau (ADR-0058).
#include "victus/graph/graph.hpp"
#include "victus/graph/material_graph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <tuple>

namespace victus::graph {

using json::Value;

std::string_view pin_type_name(PinType t) noexcept {
    switch (t) {
    case PinType::Exec: return "exec";
    case PinType::Any: return "any";
    case PinType::Number: return "number";
    case PinType::Bool: return "bool";
    case PinType::String: return "string";
    case PinType::Vector: return "vector";
    case PinType::Entity: return "entity";
    case PinType::Vec2: return "vec2";
    case PinType::Vec4: return "vec4";
    }
    return "any";
}

bool parse_pin_type(std::string_view name, PinType& out) noexcept {
    for (PinType t : {PinType::Exec, PinType::Any, PinType::Number, PinType::Bool, PinType::String, PinType::Vector,
                      PinType::Entity, PinType::Vec2, PinType::Vec4})
        if (pin_type_name(t) == name) {
            out = t;
            return true;
        }
    return false;
}

bool pin_types_compatible(PinType from, PinType to, GraphDomain domain) noexcept {
    if (from == PinType::Exec || to == PinType::Exec) return from == to;
    if (domain == GraphDomain::Material) {
        const auto numeric = [](PinType t) {
            return t == PinType::Any || t == PinType::Number || t == PinType::Vec2 || t == PinType::Vector || t == PinType::Vec4;
        };
        return from == to || (numeric(from) && numeric(to));
    }
    return from == to || from == PinType::Any || to == PinType::Any;
}

namespace {

PinDesc make_pin(std::string name, PinType type, Value def = {}) { return {std::move(name), type, std::move(def)}; }
PinDesc exec_pin(std::string name) { return {std::move(name), PinType::Exec, {}}; }
Value vec(double x, double y, double z) { return Value(json::Array{Value(x), Value(y), Value(z)}); }

std::vector<NodeDesc> build_library() {
    using K = NodeKind;
    using T = PinType;
    std::vector<NodeDesc> L;
    auto event = [&](std::string type, std::string title, std::string method, std::vector<PinDesc> params, std::string tip) {
        std::vector<PinDesc> outs{exec_pin("out")};
        for (auto& p : params) outs.push_back(std::move(p));
        L.push_back({std::move(type), "Events", std::move(title), std::move(tip), K::Event, {}, std::move(outs), std::move(method), {}, {}});
    };
    event("event.start", "On Start", "start", {}, "Runs once before the entity's first update");
    event("event.update", "On Update", "update", {make_pin("dt", T::Number)}, "Runs every frame; dt is the frame time in seconds");
    event("event.fixed_update", "On Fixed Update", "fixed_update", {make_pin("dt", T::Number)}, "Runs at the simulation's fixed rate");
    event("event.collision_enter", "On Collision Enter", "on_collision_enter", {make_pin("other", T::Entity)}, "A physics body started touching this one");
    event("event.collision_exit", "On Collision Exit", "on_collision_exit", {make_pin("other", T::Entity)}, "A physics body stopped touching this one");
    event("event.trigger_enter", "On Trigger Enter", "on_trigger_enter", {make_pin("other", T::Entity)}, "A body entered this trigger");
    event("event.trigger_exit", "On Trigger Exit", "on_trigger_exit", {make_pin("other", T::Entity)}, "A body left this trigger");

    auto flow = [&](std::string type, std::string title, K kind, std::vector<PinDesc> ins, std::vector<PinDesc> outs, std::string tip) {
        L.push_back({std::move(type), "Flow", std::move(title), std::move(tip), kind, std::move(ins), std::move(outs), {}, {}, {}});
    };
    flow("flow.branch", "Branch", K::Branch, {exec_pin("in"), make_pin("condition", T::Bool, true)}, {exec_pin("true"), exec_pin("false")},
         "Runs one of two paths");
    flow("flow.sequence", "Sequence", K::Sequence, {exec_pin("in")}, {exec_pin("then_0"), exec_pin("then_1"), exec_pin("then_2")},
         "Runs each output in order");
    flow("flow.for_range", "For Range", K::ForRange, {exec_pin("in"), make_pin("first", T::Number, 1), make_pin("last", T::Number, 10)},
         {exec_pin("body"), make_pin("index", T::Number), exec_pin("completed")}, "Runs body for each index from first to last (inclusive)");
    flow("flow.do_once", "Do Once", K::DoOnce, {exec_pin("in")}, {exec_pin("out")}, "Lets execution through only the first time (per entity)");
    L.push_back({"variable.get", "Variables", "Get Variable", "Reads a graph variable", K::GetVariable, {}, {make_pin("value", T::Any)}, {}, {}, {}});
    L.push_back({"variable.set", "Variables", "Set Variable", "Writes a graph variable", K::SetVariable,
                 {exec_pin("in"), make_pin("value", T::Any)}, {exec_pin("out"), make_pin("value", T::Any)}, {}, {}, {}});

    auto stmt = [&](std::string type, std::string category, std::string title, std::vector<PinDesc> ins, std::vector<PinDesc> outs,
                    std::string code, std::string tip) {
        ins.insert(ins.begin(), exec_pin("in"));
        outs.insert(outs.begin(), exec_pin("out"));
        L.push_back({std::move(type), std::move(category), std::move(title), std::move(tip), K::Statement, std::move(ins),
                     std::move(outs), {}, std::move(code), {}});
    };
    const PinDesc self_entity = make_pin("entity", T::Entity, "self");
    stmt("debug.log", "Debug", "Log", {make_pin("message", T::Any, "Hello")}, {}, "print(tostring({message}))", "Writes to the log (channel script)");
    stmt("entity.set_position", "Entity", "Set Position", {self_entity, make_pin("position", T::Vector, vec(0, 0, 0))}, {},
         "{entity}.position = {position}", "Moves the entity to a world position");
    stmt("entity.translate", "Entity", "Translate", {self_entity, make_pin("delta", T::Vector, vec(0, 0, 0))}, {},
         "{entity}.position = {entity}.position + {delta}", "Moves the entity by an offset");
    stmt("entity.set_rotation", "Entity", "Set Rotation", {self_entity, make_pin("degrees", T::Vector, vec(0, 0, 0))}, {},
         "{entity}.rotation = {degrees}", "Sets the rotation as Euler angles in degrees");
    stmt("entity.set_scale", "Entity", "Set Scale", {self_entity, make_pin("scale", T::Vector, vec(1, 1, 1))}, {}, "{entity}.scale = {scale}",
         "Sets the scale");
    stmt("entity.set_visible", "Entity", "Set Visible", {self_entity, make_pin("visible", T::Bool, true)}, {},
         "{entity}:set_visible({visible})", "Shows or hides the entity's mesh or sprite");
    stmt("entity.set_color", "Entity", "Set Color", {self_entity, make_pin("color", T::Vector, vec(1, 1, 1))}, {},
         "{entity}:set_color({color})", "Tints the entity's mesh or sprite");
    stmt("entity.add_impulse", "Physics", "Add Impulse", {self_entity, make_pin("impulse", T::Vector, vec(0, 5, 0))}, {},
         "{entity}:add_impulse({impulse})", "Kicks a dynamic body");
    stmt("entity.add_force", "Physics", "Add Force", {self_entity, make_pin("force", T::Vector, vec(0, 0, 0))}, {},
         "{entity}:add_force({force})", "Pushes a dynamic body this step");
    stmt("entity.set_destination", "Navigation", "Set Destination", {self_entity, make_pin("target", T::Vector, vec(0, 0, 0))}, {},
         "{entity}:set_destination({target})", "Sends a NavAgent towards a point");
    stmt("entity.destroy", "Entity", "Destroy", {self_entity}, {}, "scene.destroy({entity})", "Removes the entity from the scene");
    stmt("scene.spawn", "Scene", "Spawn Entity", {make_pin("name", T::String, "Entity")}, {make_pin("entity", T::Entity)},
         "{=entity} = scene.spawn({name})", "Creates an empty entity");
    stmt("scene.instantiate", "Scene", "Instantiate Prefab", {make_pin("prefab", T::String, ""), make_pin("position", T::Vector, vec(0, 0, 0))},
         {make_pin("entity", T::Entity)}, "{=entity} = scene.instantiate({prefab}, { position = {position} })",
         "Creates an instance of a .vprefab asset");
    stmt("scene.load", "Scene", "Load Scene", {make_pin("path", T::String, "scenes/main.vscene")}, {}, "scene.load({path})",
         "Switches to another scene at the end of the update");
    stmt("audio.play", "Audio", "Play Sound", {make_pin("clip", T::String, "")}, {}, "audio.play({clip})", "Plays a one-shot sound");
    stmt("entity.play_audio", "Audio", "Play Audio Source", {self_entity}, {}, "{entity}:play_audio()", "Plays the entity's AudioSource");

    auto pure = [&](std::string type, std::string category, std::string title, std::vector<PinDesc> ins, std::vector<PinDesc> outs,
                    std::vector<std::string> code, std::string tip = {}) {
        L.push_back({std::move(type), std::move(category), std::move(title), std::move(tip), K::Pure, std::move(ins), std::move(outs), {},
                     {}, std::move(code)});
    };
    pure("const.number", "Values", "Number", {make_pin("value", T::Number, 0)}, {make_pin("value", T::Number)}, {"{value}"});
    pure("const.bool", "Values", "Boolean", {make_pin("value", T::Bool, false)}, {make_pin("value", T::Bool)}, {"{value}"});
    pure("const.string", "Values", "Text", {make_pin("value", T::String, "")}, {make_pin("value", T::String)}, {"{value}"});
    pure("const.vector", "Values", "Vector", {make_pin("value", T::Vector, vec(0, 0, 0))}, {make_pin("value", T::Vector)}, {"{value}"});
    pure("entity.self", "Entity", "Self", {}, {make_pin("entity", T::Entity)}, {"{self}.entity"}, "The entity running this graph");
    pure("entity.find", "Entity", "Find Entity", {make_pin("name", T::String, "")}, {make_pin("entity", T::Entity)}, {"scene.find({name})"},
         "First entity with this name (nil when none)");
    pure("entity.position", "Entity", "Get Position", {self_entity}, {make_pin("position", T::Vector)}, {"{entity}.position"});
    pure("entity.rotation", "Entity", "Get Rotation", {self_entity}, {make_pin("degrees", T::Vector)}, {"{entity}.rotation"});
    pure("entity.scale", "Entity", "Get Scale", {self_entity}, {make_pin("scale", T::Vector)}, {"{entity}.scale"});
    pure("entity.forward", "Entity", "Get Forward", {self_entity}, {make_pin("forward", T::Vector)}, {"{entity}.forward"});
    pure("entity.name", "Entity", "Get Name", {self_entity}, {make_pin("name", T::String)}, {"{entity}.name"});
    pure("entity.valid", "Entity", "Is Valid", {self_entity}, {make_pin("valid", T::Bool)}, {"({entity} ~= nil and {entity}.valid)"},
         "False for nil or destroyed entities");
    pure("input.action", "Input", "Action Held", {make_pin("action", T::String, "jump")}, {make_pin("held", T::Bool)}, {"input.action({action})"});
    pure("input.action_pressed", "Input", "Action Pressed", {make_pin("action", T::String, "jump")}, {make_pin("pressed", T::Bool)},
         {"input.action_pressed({action})"}, "True on the frame the action started");
    pure("input.action_released", "Input", "Action Released", {make_pin("action", T::String, "jump")}, {make_pin("released", T::Bool)},
         {"input.action_released({action})"});
    pure("input.value", "Input", "Action Value", {make_pin("action", T::String, "move_x")}, {make_pin("value", T::Number)},
         {"input.value({action})"}, "-1..1 for axis actions");
    pure("input.axis", "Input", "Key Axis", {make_pin("negative", T::String, "A"), make_pin("positive", T::String, "D")}, {make_pin("value", T::Number)},
         {"input.axis({negative}, {positive})"});
    pure("input.key_down", "Input", "Key Held", {make_pin("key", T::String, "Space")}, {make_pin("held", T::Bool)}, {"input.down({key})"});
    pure("input.key_pressed", "Input", "Key Pressed", {make_pin("key", T::String, "Space")}, {make_pin("pressed", T::Bool)},
         {"input.pressed({key})"});
    auto binary = [&](std::string type, std::string title, std::string op, T in, T out, Value def = 0) {
        pure(std::move(type), out == T::Bool && in != T::Bool ? "Compare" : (in == T::Bool ? "Logic" : "Math"), std::move(title),
             {make_pin("a", in, def), make_pin("b", in, def)}, {make_pin("result", out)}, {"({a} " + op + " {b})"});
    };
    binary("math.add", "Add", "+", T::Any, T::Any);
    binary("math.subtract", "Subtract", "-", T::Any, T::Any);
    binary("math.multiply", "Multiply", "*", T::Any, T::Any, 1);
    binary("math.divide", "Divide", "/", T::Any, T::Any, 1);
    binary("math.modulo", "Modulo", "%", T::Number, T::Number, 1);
    binary("math.power", "Power", "^", T::Number, T::Number, 1);
    binary("math.greater", "Greater", ">", T::Number, T::Bool);
    binary("math.less", "Less", "<", T::Number, T::Bool);
    binary("math.greater_equal", "Greater Or Equal", ">=", T::Number, T::Bool);
    binary("math.less_equal", "Less Or Equal", "<=", T::Number, T::Bool);
    binary("math.equal", "Equal", "==", T::Any, T::Bool);
    binary("math.not_equal", "Not Equal", "~=", T::Any, T::Bool);
    binary("logic.and", "And", "and", T::Bool, T::Bool, false);
    binary("logic.or", "Or", "or", T::Bool, T::Bool, false);
    pure("logic.not", "Logic", "Not", {make_pin("value", T::Bool, false)}, {make_pin("result", T::Bool)}, {"(not {value})"});
    pure("math.negate", "Math", "Negate", {make_pin("value", T::Any, 0)}, {make_pin("result", T::Any)}, {"(-{value})"});
    auto unary = [&](std::string type, std::string title, std::string fn) {
        pure(std::move(type), "Math", std::move(title), {make_pin("value", T::Number, 0)}, {make_pin("result", T::Number)}, {fn + "({value})"});
    };
    unary("math.abs", "Absolute", "math.abs");
    unary("math.floor", "Floor", "math.floor");
    unary("math.ceil", "Ceil", "math.ceil");
    unary("math.sqrt", "Square Root", "math.sqrt");
    unary("math.sin", "Sine", "math.sin");
    unary("math.cos", "Cosine", "math.cos");
    unary("math.radians", "To Radians", "math.rad");
    pure("math.min", "Math", "Min", {make_pin("a", T::Number, 0), make_pin("b", T::Number, 0)}, {make_pin("result", T::Number)}, {"math.min({a}, {b})"});
    pure("math.max", "Math", "Max", {make_pin("a", T::Number, 0), make_pin("b", T::Number, 0)}, {make_pin("result", T::Number)}, {"math.max({a}, {b})"});
    pure("math.clamp", "Math", "Clamp", {make_pin("value", T::Number, 0), make_pin("min", T::Number, 0), make_pin("max", T::Number, 1)},
         {make_pin("result", T::Number)}, {"math.clamp({value}, {min}, {max})"});
    pure("math.lerp", "Math", "Lerp", {make_pin("a", T::Any, 0), make_pin("b", T::Any, 1), make_pin("t", T::Number, 0.5)}, {make_pin("result", T::Any)},
         {"({a} + ({b} - {a}) * {t})"});
    pure("math.random_range", "Math", "Random Range", {make_pin("min", T::Number, 0), make_pin("max", T::Number, 1)}, {make_pin("result", T::Number)},
         {"({min} + math.random() * ({max} - {min}))"}, "A new random number every time it is read");
    pure("vector.make", "Vector", "Make Vector", {make_pin("x", T::Number, 0), make_pin("y", T::Number, 0), make_pin("z", T::Number, 0)},
         {make_pin("vector", T::Vector)}, {"vector.create({x}, {y}, {z})"});
    pure("vector.split", "Vector", "Split Vector", {make_pin("vector", T::Vector, vec(0, 0, 0))},
         {make_pin("x", T::Number), make_pin("y", T::Number), make_pin("z", T::Number)}, {"{vector}.x", "{vector}.y", "{vector}.z"});
    pure("vector.length", "Vector", "Length", {make_pin("vector", T::Vector, vec(0, 0, 0))}, {make_pin("length", T::Number)},
         {"vector.magnitude({vector})"});
    pure("vector.normalize", "Vector", "Normalize", {make_pin("vector", T::Vector, vec(0, 0, 1))}, {make_pin("vector", T::Vector)},
         {"vector.normalize({vector})"});
    pure("vector.dot", "Vector", "Dot", {make_pin("a", T::Vector, vec(0, 0, 0)), make_pin("b", T::Vector, vec(0, 0, 0))}, {make_pin("result", T::Number)},
         {"vector.dot({a}, {b})"});
    pure("vector.cross", "Vector", "Cross", {make_pin("a", T::Vector, vec(0, 0, 0)), make_pin("b", T::Vector, vec(0, 0, 0))},
         {make_pin("result", T::Vector)}, {"vector.cross({a}, {b})"});
    pure("vector.distance", "Vector", "Distance", {make_pin("a", T::Vector, vec(0, 0, 0)), make_pin("b", T::Vector, vec(0, 0, 0))},
         {make_pin("distance", T::Number)}, {"vector.magnitude({a} - {b})"});
    pure("string.concat", "Text", "Join Text", {make_pin("a", T::Any, ""), make_pin("b", T::Any, "")}, {make_pin("text", T::String)},
         {"(tostring({a}) .. tostring({b}))"});
    pure("string.from", "Text", "To Text", {make_pin("value", T::Any, 0)}, {make_pin("text", T::String)}, {"tostring({value})"});
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

bool is_identifier(std::string_view s) {
    if (s.empty() || s.size() > 64 || !(std::isalpha(static_cast<unsigned char>(s[0])))) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    static const std::set<std::string_view> reserved = {
        "and",    "break", "do",     "else",   "elseif", "end",       "false",      "for",   "function",   "if",
        "in",     "local", "nil",    "not",    "or",     "repeat",    "return",     "then",  "true",       "until",
        "while",  "continue", "type", "export", "entity", "start",    "update",     "fixed_update", "reload", "save_state",
        "load_state"};
    if (reserved.contains(s)) return false;
    return !s.starts_with("on_");
}

std::string number_literal(double d) {
    if (!std::isfinite(d)) d = 0.0;
    if (d == std::floor(d) && std::abs(d) < 1e15) {
        const auto i = static_cast<long long>(d);
        return i < 0 ? std::format("({})", i) : std::format("{}", i);
    }
    std::string s = std::format("{}", d);
    return d < 0 ? "(" + s + ")" : s;
}

std::string string_literal(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) out += std::format("\\{:03d}", static_cast<int>(c));
            else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::string vector_literal(const Value& v) {
    double c[3] = {0, 0, 0};
    if (v.is_array())
        for (std::size_t i = 0; i < 3 && i < v.size(); ++i) c[i] = v[i].as_number();
    else if (v.is_number())
        c[0] = c[1] = c[2] = v.as_number();
    return std::format("vector.create({}, {}, {})", number_literal(c[0]), number_literal(c[1]), number_literal(c[2]));
}

std::string literal(PinType type, const Value& v) {
    switch (type) {
    case PinType::Number: return number_literal(v.as_number());
    case PinType::Bool: return v.as_bool() ? "true" : "false";
    case PinType::String: return string_literal(v.is_string() ? v.as_string() : std::string_view{});
    case PinType::Vector: return vector_literal(v);
    case PinType::Entity:
        if (v.is_string() && v.as_string() == "self") return "self.entity";
        if (v.is_string() && !v.as_string().empty()) return "scene.find(" + string_literal(v.as_string()) + ")";
        return "nil";
    case PinType::Any:
        if (v.is_number()) return number_literal(v.as_number());
        if (v.is_bool()) return v.as_bool() ? "true" : "false";
        if (v.is_string()) return string_literal(v.as_string());
        if (v.is_array()) return vector_literal(v);
        return "nil";
    case PinType::Vec2:
    case PinType::Vec4:
    case PinType::Exec: break;
    }
    return "nil";
}

std::string variable_field(const GraphVariable& v) { return v.exposed ? v.name : "_v_" + v.name; }

} // namespace

std::span<const NodeDesc> node_library() { return library(); }

std::span<const NodeDesc> node_library(GraphDomain domain) {
    return domain == GraphDomain::Material ? material_node_library() : node_library();
}

const NodeDesc* find_node_desc(std::string_view type, GraphDomain domain) {
    for (const auto& d : node_library(domain))
        if (d.type == type) return &d;
    return nullptr;
}

const NodeDesc* find_node_desc(std::string_view type) {
    if (const NodeDesc* d = find_node_desc(type, GraphDomain::Script)) return d;
    return find_node_desc(type, GraphDomain::Material);
}

// ---- graph editing ----

GraphNode* ScriptGraph::node(std::uint32_t id) noexcept {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const GraphNode* ScriptGraph::node(std::uint32_t id) const noexcept {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const GraphVariable* ScriptGraph::variable(std::string_view name) const noexcept {
    for (const auto& v : variables)
        if (v.name == name) return &v;
    return nullptr;
}

std::uint32_t ScriptGraph::add_node(std::string type, float x, float y) {
    for (const auto& n : nodes) next_id = std::max(next_id, n.id + 1);
    GraphNode n;
    n.id = next_id++;
    n.type = std::move(type);
    n.x = x;
    n.y = y;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

void ScriptGraph::remove_node(std::uint32_t id) {
    std::erase_if(nodes, [&](const GraphNode& n) { return n.id == id; });
    std::erase_if(links, [&](const GraphLink& l) { return l.from_node == id || l.to_node == id; });
}

std::vector<PinDesc> ScriptGraph::input_pins(const GraphNode& n) const {
    const NodeDesc* d = find_node_desc(n.type, domain);
    if (!d) return {};
    std::vector<PinDesc> pins = d->inputs;
    if (d->kind == NodeKind::SetVariable)
        if (const GraphVariable* v = variable(n.config["variable"].as_string_or(""))) {
            pins[1].type = v->type;
            pins[1].default_value = v->default_value;
        }
    return pins;
}

std::vector<PinDesc> ScriptGraph::output_pins(const GraphNode& n) const {
    const NodeDesc* d = find_node_desc(n.type, domain);
    if (!d) return {};
    std::vector<PinDesc> pins = d->outputs;
    if (d->kind == NodeKind::GetVariable || d->kind == NodeKind::SetVariable)
        if (const GraphVariable* v = variable(n.config["variable"].as_string_or("")))
            pins.back().type = v->type;
    return pins;
}

bool ScriptGraph::connect(std::uint32_t from_node, std::string_view from_pin, std::uint32_t to_node, std::string_view to_pin) {
    const GraphNode* a = node(from_node);
    const GraphNode* b = node(to_node);
    if (!a || !b || from_node == to_node) return false;
    const auto outs = output_pins(*a);
    const auto ins = input_pins(*b);
    const PinDesc* o = find_pin(outs, from_pin);
    const PinDesc* i = find_pin(ins, to_pin);
    if (!o || !i || !pin_types_compatible(o->type, i->type, domain)) return false;
    if (o->type == PinType::Exec)
        std::erase_if(links, [&](const GraphLink& l) { return l.from_node == from_node && l.from_pin == from_pin; });
    else
        std::erase_if(links, [&](const GraphLink& l) { return l.to_node == to_node && l.to_pin == to_pin; });
    links.push_back({from_node, std::string(from_pin), to_node, std::string(to_pin)});
    return true;
}

void ScriptGraph::disconnect_input(std::uint32_t id, std::string_view pin_name) {
    std::erase_if(links, [&](const GraphLink& l) { return l.to_node == id && l.to_pin == pin_name; });
}

// ---- serialization ----

json::Value graph_to_json(const ScriptGraph& g) {
    Value doc = Value::object();
    doc.set("format", "victus.graph");
    doc.set("version", ScriptGraph::kVersion);
    if (g.domain == GraphDomain::Material) doc.set("domain", "material");
    std::uint32_t next = g.next_id;
    for (const auto& n : g.nodes) next = std::max(next, n.id + 1);
    doc.set("next_id", next);
    Value vars = Value::array();
    for (const auto& v : g.variables) {
        Value o = Value::object();
        o.set("name", v.name);
        o.set("type", pin_type_name(v.type));
        o.set("default", v.default_value);
        o.set("exposed", v.exposed);
        vars.push(std::move(o));
    }
    doc.set("variables", std::move(vars));
    std::vector<const GraphNode*> nodes;
    for (const auto& n : g.nodes) nodes.push_back(&n);
    std::sort(nodes.begin(), nodes.end(), [](const GraphNode* a, const GraphNode* b) { return a->id < b->id; });
    Value nv = Value::array();
    for (const GraphNode* n : nodes) {
        Value o = Value::object();
        o.set("id", n->id);
        o.set("type", n->type);
        o.set("position", Value(json::Array{Value(std::round(n->x * 100.0) / 100.0), Value(std::round(n->y * 100.0) / 100.0)}));
        if (n->inputs.is_object() && n->inputs.size() > 0) {
            // Keys sorted so editing order does not change the file.
            json::Object sorted = n->inputs.as_object();
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            o.set("inputs", Value(std::move(sorted)));
        }
        if (n->config.is_object() && n->config.size() > 0) {
            json::Object sorted = n->config.as_object();
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            o.set("config", Value(std::move(sorted)));
        }
        if (!n->comment.empty()) o.set("comment", n->comment);
        nv.push(std::move(o));
    }
    doc.set("nodes", std::move(nv));
    std::vector<GraphLink> links = g.links;
    std::sort(links.begin(), links.end(), [](const GraphLink& a, const GraphLink& b) {
        return std::tie(a.from_node, a.from_pin, a.to_node, a.to_pin) < std::tie(b.from_node, b.from_pin, b.to_node, b.to_pin);
    });
    links.erase(std::unique(links.begin(), links.end()), links.end());
    Value lv = Value::array();
    for (const auto& l : links) {
        Value o = Value::object();
        o.set("from", Value(json::Array{Value(l.from_node), Value(l.from_pin)}));
        o.set("to", Value(json::Array{Value(l.to_node), Value(l.to_pin)}));
        lv.push(std::move(o));
    }
    doc.set("links", std::move(lv));
    return doc;
}

Result<ScriptGraph> graph_from_json(const json::Value& doc) {
    if (!doc.is_object() || doc["format"].as_string_or("") != "victus.graph")
        return make_error(ErrorCode::ParseError, "not a visual script graph (format must be \"victus.graph\")");
    const auto version = doc["version"].as_int(0);
    if (version < 1 || version > ScriptGraph::kVersion)
        return make_error(ErrorCode::VersionMismatch, std::format("unsupported graph version {}", version));
    ScriptGraph g;
    const std::string domain = doc["domain"].as_string_or("script");
    if (domain == "material") g.domain = GraphDomain::Material;
    else if (domain != "script") return make_error(ErrorCode::ParseError, std::format("unknown graph domain '{}'", domain));
    constexpr std::size_t kMaxNodes = 20000;
    for (const Value& v : doc["variables"].is_array() ? doc["variables"].as_array() : json::Array{}) {
        GraphVariable var;
        var.name = v["name"].as_string_or("");
        if (!parse_pin_type(v["type"].as_string_or("number"), var.type)) var.type = PinType::Any;
        var.default_value = v["default"];
        var.exposed = v["exposed"].as_bool(true);
        g.variables.push_back(std::move(var));
    }
    const Value& nodes = doc["nodes"];
    if (nodes.is_array()) {
        if (nodes.size() > kMaxNodes) return make_error(ErrorCode::LimitExceeded, "graph has too many nodes");
        for (const Value& v : nodes.as_array()) {
            GraphNode n;
            const auto id = v["id"].as_int(0);
            if (id <= 0 || id > 0xffffffffll) return make_error(ErrorCode::ParseError, "graph node without a valid id");
            n.id = static_cast<std::uint32_t>(id);
            n.type = v["type"].as_string_or("");
            n.x = v["position"][0].as_float();
            n.y = v["position"][1].as_float();
            if (v["inputs"].is_object()) n.inputs = v["inputs"];
            if (v["config"].is_object()) n.config = v["config"];
            n.comment = v["comment"].as_string_or("");
            g.nodes.push_back(std::move(n));
        }
    }
    const Value& links = doc["links"];
    if (links.is_array())
        for (const Value& v : links.as_array()) {
            GraphLink l;
            l.from_node = static_cast<std::uint32_t>(std::max<std::int64_t>(v["from"][0].as_int(0), 0));
            l.from_pin = v["from"][1].as_string_or("");
            l.to_node = static_cast<std::uint32_t>(std::max<std::int64_t>(v["to"][0].as_int(0), 0));
            l.to_pin = v["to"][1].as_string_or("");
            g.links.push_back(std::move(l));
        }
    g.next_id = static_cast<std::uint32_t>(std::clamp<std::int64_t>(doc["next_id"].as_int(1), 1, 0xffffffffll));
    for (const auto& n : g.nodes) g.next_id = std::max(g.next_id, n.id + 1);
    return g;
}

Result<ScriptGraph> parse_graph(std::string_view text) {
    auto doc = json::parse(text);
    if (!doc) return std::move(doc).error();
    return graph_from_json(*doc);
}

std::string serialize_graph(const ScriptGraph& graph) { return json::stringify(graph_to_json(graph), true) + "\n"; }

// ---- compiler ----

bool CompiledGraph::ok() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const GraphDiagnostic& d) { return d.severity == GraphDiagnostic::Severity::Error; });
}

std::uint32_t CompiledGraph::node_at_line(std::uint32_t line) const noexcept {
    return line >= 1 && line <= line_nodes.size() ? line_nodes[line - 1] : 0;
}

std::string CompiledGraph::error_text() const {
    std::string out;
    for (const auto& d : diagnostics) {
        if (d.severity != GraphDiagnostic::Severity::Error) continue;
        if (!out.empty()) out += "\n";
        out += d.node ? std::format("node {}{}: {}", d.node, d.pin.empty() ? "" : " pin " + d.pin, d.message) : d.message;
    }
    return out;
}

namespace {

class Compiler {
public:
    Compiler(const ScriptGraph& g, const GraphCompileOptions& o) : g_(g), opt_(o) {}

    CompiledGraph run() {
        if (g_.domain != GraphDomain::Script) {
            error(0, {}, "this is a material graph: it compiles to a surface shader, not a script");
            return finish(false);
        }
        index();
        check_variables();
        check_links();
        if (has_errors()) return finish(false);
        emit_module();
        if (lines_ > kMaxLines) error(0, {}, "the generated script is too large (deeply duplicated branches?)");
        return finish(!has_errors());
    }

private:
    static constexpr std::size_t kMaxLines = 200000;
    struct Out {
        std::uint32_t node;
        std::string pin;
    };
    const ScriptGraph& g_;
    const GraphCompileOptions& opt_;
    std::vector<GraphDiagnostic> diags_;
    std::map<std::uint32_t, const GraphNode*> nodes_;
    std::map<std::uint32_t, const NodeDesc*> descs_;
    std::map<std::pair<std::uint32_t, std::string>, Out> data_in_;          // (node, input) -> source output
    std::map<std::pair<std::uint32_t, std::string>, std::uint32_t> exec_out_; // (node, exec output) -> target
    std::set<std::uint32_t> reached_any_;
    // Per event function.
    std::set<std::uint32_t> reach_;
    std::vector<std::uint32_t> path_;
    std::set<std::uint32_t> expr_stack_;
    std::string text_;
    std::vector<std::uint32_t> line_nodes_;
    std::size_t lines_{0};
    int indent_{0};

    void error(std::uint32_t node, std::string pin, std::string msg) {
        diags_.push_back({GraphDiagnostic::Severity::Error, node, std::move(pin), std::move(msg)});
    }
    void warn(std::uint32_t node, std::string msg) { diags_.push_back({GraphDiagnostic::Severity::Warning, node, {}, std::move(msg)}); }
    bool has_errors() const {
        return std::any_of(diags_.begin(), diags_.end(), [](const auto& d) { return d.severity == GraphDiagnostic::Severity::Error; });
    }

    CompiledGraph finish(bool ok) {
        CompiledGraph c;
        c.diagnostics = std::move(diags_);
        if (ok) {
            c.source = std::move(text_);
            c.line_nodes = std::move(line_nodes_);
        }
        return c;
    }

    void line(std::string_view s, std::uint32_t node = 0) {
        ++lines_;
        if (lines_ > kMaxLines) return;
        text_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        text_ += s;
        text_ += '\n';
        line_nodes_.push_back(node);
    }

    void index() {
        for (const auto& n : g_.nodes) {
            if (n.id == 0) error(0, {}, "node with id 0");
            if (!nodes_.emplace(n.id, &n).second) error(n.id, {}, "duplicate node id");
            const NodeDesc* d = find_node_desc(n.type, GraphDomain::Script);
            if (!d) error(n.id, {}, std::format("unknown node type '{}'", n.type));
            else descs_[n.id] = d;
        }
        std::set<std::string> events;
        for (const auto& [id, d] : descs_) {
            if (d->kind == NodeKind::Event && !events.insert(d->type).second)
                error(id, {}, std::format("'{}' appears more than once; join its paths with a Sequence", d->title));
            if (d->kind == NodeKind::GetVariable || d->kind == NodeKind::SetVariable) {
                const std::string name = nodes_[id]->config["variable"].as_string_or("");
                if (!g_.variable(name)) error(id, {}, name.empty() ? "no variable selected" : std::format("unknown variable '{}'", name));
            }
        }
    }

    void check_variables() {
        std::set<std::string> seen;
        for (const auto& v : g_.variables) {
            if (!is_identifier(v.name))
                error(0, {}, std::format("variable name '{}' must start with a letter, use letters, digits and _ only, and not be a "
                                         "Luau keyword or a script callback name", v.name));
            if (!seen.insert(v.name).second) error(0, {}, std::format("variable '{}' is declared twice", v.name));
            if (v.type == PinType::Exec) error(0, {}, std::format("variable '{}' cannot have type exec", v.name));
        }
    }

    void check_links() {
        for (const auto& l : g_.links) {
            const auto a = nodes_.find(l.from_node), b = nodes_.find(l.to_node);
            if (a == nodes_.end() || b == nodes_.end()) {
                error(a == nodes_.end() ? 0 : l.from_node, {}, std::format("link to missing node {}", a == nodes_.end() ? l.from_node : l.to_node));
                continue;
            }
            if (!descs_.contains(l.from_node) || !descs_.contains(l.to_node)) continue; // already reported
            const auto outs = g_.output_pins(*a->second);
            const auto ins = g_.input_pins(*b->second);
            const PinDesc* o = find_pin(outs, l.from_pin);
            const PinDesc* i = find_pin(ins, l.to_pin);
            if (!o) {
                error(l.from_node, l.from_pin, "link from a pin this node does not have");
                continue;
            }
            if (!i) {
                error(l.to_node, l.to_pin, "link into a pin this node does not have");
                continue;
            }
            if (!pin_types_compatible(o->type, i->type)) {
                error(l.to_node, l.to_pin,
                      std::format("cannot connect {} output '{}' to {} input", pin_type_name(o->type), l.from_pin, pin_type_name(i->type)));
                continue;
            }
            if (o->type == PinType::Exec) {
                if (!exec_out_.emplace(std::pair{l.from_node, l.from_pin}, l.to_node).second)
                    error(l.from_node, l.from_pin, "an exec output can only connect to one node (use a Sequence)");
            } else if (!data_in_.emplace(std::pair{l.to_node, l.to_pin}, Out{l.from_node, l.from_pin}).second) {
                error(l.to_node, l.to_pin, "a data input can only have one link");
            }
        }
    }

    std::string local_name(std::uint32_t node, std::string_view pin_name) const { return std::format("n{}_{}", node, pin_name); }

    std::string field(const GraphNode& n) const {
        const GraphVariable* v = g_.variable(n.config["variable"].as_string_or(""));
        return "self." + (v ? variable_field(*v) : std::string("_missing"));
    }

    // Expression for input `pin` of node `id`.
    std::string input_expr(std::uint32_t id, const PinDesc& p) {
        if (auto it = data_in_.find({id, p.name}); it != data_in_.end()) return output_expr(it->second.node, it->second.pin, id);
        const GraphNode& n = *nodes_.at(id);
        const Value* lit = n.inputs.find(p.name);
        return literal(p.type, lit ? *lit : p.default_value);
    }

    std::string output_expr(std::uint32_t id, const std::string& pin_name, std::uint32_t user) {
        const NodeDesc& d = *descs_.at(id);
        const GraphNode& n = *nodes_.at(id);
        switch (d.kind) {
        case NodeKind::Pure: {
            if (expr_stack_.contains(id)) {
                error(id, pin_name, "data loop: this value depends on itself");
                return "nil";
            }
            expr_stack_.insert(id);
            std::size_t idx = 0;
            while (idx < d.outputs.size() && d.outputs[idx].name != pin_name) ++idx;
            std::string out = idx < d.output_code.size() ? expand(id, d.output_code[idx]) : "nil";
            expr_stack_.erase(id);
            return out;
        }
        case NodeKind::GetVariable:
        case NodeKind::SetVariable: return field(n);
        case NodeKind::Event:
        case NodeKind::Statement:
        case NodeKind::ForRange:
            if (!reach_.contains(id)) {
                error(user, {}, std::format("uses '{}' from node {}, which does not run in the same event", pin_name, id));
                return "nil";
            }
            return local_name(id, pin_name);
        default: return "nil";
        }
    }

    // Expands `{pin}`, `{=pin}` and `{self}` in a template.
    std::string expand(std::uint32_t id, std::string_view tmpl) {
        const GraphNode& n = *nodes_.at(id);
        const auto ins = g_.input_pins(n);
        std::string out;
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] == '{') {
                std::size_t j = i + 1;
                const bool is_out = j < tmpl.size() && tmpl[j] == '=';
                if (is_out) ++j;
                std::size_t k = j;
                while (k < tmpl.size() && (std::isalnum(static_cast<unsigned char>(tmpl[k])) || tmpl[k] == '_')) ++k;
                if (k < tmpl.size() && tmpl[k] == '}' && k > j) {
                    const std::string_view name = tmpl.substr(j, k - j);
                    if (is_out) out += local_name(id, name);
                    else if (name == "self") out += "self";
                    else if (const PinDesc* p = find_pin(ins, name)) out += input_expr(id, *p);
                    else out += "nil";
                    i = k;
                    continue;
                }
            }
            out += tmpl[i];
        }
        return out;
    }

    void collect_reach(std::uint32_t id, std::set<std::uint32_t>& out) {
        if (!out.insert(id).second) return;
        for (const auto& [key, target] : exec_out_)
            if (key.first == id) collect_reach(target, out);
    }

    void follow(std::uint32_t id, const char* exec_pin) {
        if (auto it = exec_out_.find({id, exec_pin}); it != exec_out_.end()) emit_exec(it->second);
    }

    void trace(std::uint32_t id) {
        if (opt_.trace) line(std::format("__vg_trace({})", id), id);
    }

    void emit_exec(std::uint32_t id) {
        if (lines_ > kMaxLines) return;
        if (std::find(path_.begin(), path_.end(), id) != path_.end()) {
            error(id, {}, "exec loop: this node runs itself again (use For Range for repetition)");
            return;
        }
        const NodeDesc& d = *descs_.at(id);
        const GraphNode& n = *nodes_.at(id);
        reached_any_.insert(id);
        path_.push_back(id);
        switch (d.kind) {
        case NodeKind::Statement:
            trace(id);
            line(expand(id, d.code), id);
            follow(id, "out");
            break;
        case NodeKind::SetVariable: {
            trace(id);
            const auto ins = g_.input_pins(n);
            line(field(n) + " = " + input_expr(id, ins[1]), id);
            follow(id, "out");
            break;
        }
        case NodeKind::Branch: {
            trace(id);
            const auto ins = g_.input_pins(n);
            line("if " + input_expr(id, ins[1]) + " then", id);
            ++indent_;
            follow(id, "true");
            --indent_;
            if (exec_out_.contains({id, "false"})) {
                line("else", id);
                ++indent_;
                follow(id, "false");
                --indent_;
            }
            line("end", id);
            break;
        }
        case NodeKind::Sequence:
            trace(id);
            for (const char* p : {"then_0", "then_1", "then_2"}) {
                if (!exec_out_.contains({id, p})) continue;
                line("do", id);
                ++indent_;
                follow(id, p);
                --indent_;
                line("end", id);
            }
            break;
        case NodeKind::ForRange: {
            trace(id);
            const auto ins = g_.input_pins(n);
            line(std::format("for {} = {}, {} do", local_name(id, "index"), input_expr(id, ins[1]), input_expr(id, ins[2])), id);
            ++indent_;
            follow(id, "body");
            --indent_;
            line("end", id);
            follow(id, "completed");
            break;
        }
        case NodeKind::DoOnce:
            trace(id);
            line(std::format("if not self._vg_once_{} then", id), id);
            ++indent_;
            line(std::format("self._vg_once_{} = true", id), id);
            follow(id, "out");
            --indent_;
            line("end", id);
            break;
        default: error(id, {}, std::format("'{}' cannot be used in an exec chain", d.title)); break;
        }
        path_.pop_back();
    }

    void emit_module() {
        line(std::format("-- Generated from {} by VICTUS visual scripting. Edit the graph, not this file.",
                         opt_.source_name.empty() ? std::string("a graph") : opt_.source_name));
        // Hosts that debug graphs provide __vg_trace_for(graph) returning the per-graph recorder.
        if (opt_.trace)
            line(std::format("local __vg_trace = __vg_trace_for and __vg_trace_for({}) or function(_node) end",
                             string_literal(opt_.source_name)));
        line("local M = {}");
        for (const auto& v : g_.variables)
            line(std::format("M.{} = {}", variable_field(v), v.type == PinType::Entity ? std::string("nil") : literal(v.type, v.default_value)));
        // Events in library order: stable output regardless of node ids.
        for (const NodeDesc& ev : library()) {
            if (ev.kind != NodeKind::Event) continue;
            std::uint32_t id = 0;
            for (const auto& [nid, d] : descs_)
                if (d == &ev) id = nid;
            if (id == 0) continue;
            reach_.clear();
            collect_reach(id, reach_);
            std::string params;
            for (std::size_t i = 1; i < ev.outputs.size(); ++i) params += (params.empty() ? "" : ", ") + local_name(id, ev.outputs[i].name);
            line(std::format("function M:{}({})", ev.method, params), id);
            ++indent_;
            trace(id);
            std::vector<std::string> locals;
            for (std::uint32_t r : reach_) {
                const NodeDesc* d = descs_.at(r);
                if (d->kind != NodeKind::Statement) continue;
                for (const auto& p : d->outputs)
                    if (p.type != PinType::Exec) locals.push_back(local_name(r, p.name));
            }
            if (locals.size() > 150) error(id, {}, "too many node outputs in one event (limit 150)");
            for (std::size_t i = 0; i < locals.size(); i += 8) {
                std::string decl = "local ";
                for (std::size_t j = i; j < std::min(locals.size(), i + 8); ++j) decl += (j > i ? ", " : "") + locals[j];
                line(decl, id);
            }
            reached_any_.insert(id);
            path_.clear();
            follow(id, "out");
            --indent_;
            line("end", id);
        }
        line("return M");
        for (const auto& [id, d] : descs_)
            if (d->kind != NodeKind::Pure && d->kind != NodeKind::GetVariable && d->kind != NodeKind::Event && !reached_any_.contains(id))
                warn(id, std::format("'{}' is not connected to an event and never runs", d->title));
    }
};

} // namespace

CompiledGraph compile_graph(const ScriptGraph& graph, const GraphCompileOptions& options) {
    return Compiler(graph, options).run();
}

std::vector<GraphDiagnostic> validate_graph(const ScriptGraph& graph) {
    if (graph.domain == GraphDomain::Material) return validate_material_graph(graph);
    return compile_graph(graph).diagnostics;
}

std::string annotate_graph_error(std::string_view message, const CompiledGraph& compiled, const ScriptGraph& graph) {
    // Luau locations look like "scripts/a.vgraph:12: message"; find the first ":<digits>:".
    for (std::size_t i = 0; i + 1 < message.size(); ++i) {
        if (message[i] != ':' || !std::isdigit(static_cast<unsigned char>(message[i + 1]))) continue;
        std::size_t j = i + 1;
        std::uint32_t line = 0;
        while (j < message.size() && std::isdigit(static_cast<unsigned char>(message[j])) && line < 10000000)
            line = line * 10 + static_cast<std::uint32_t>(message[j++] - '0');
        if (j >= message.size() || message[j] != ':') continue;
        const std::uint32_t node = compiled.node_at_line(line);
        if (node == 0) return std::string(message);
        const GraphNode* n = graph.node(node);
        const NodeDesc* d = n ? find_node_desc(n->type) : nullptr;
        return std::format("{} [node {}{}]", message, node, d ? " " + d->title : std::string());
    }
    return std::string(message);
}

} // namespace victus::graph
