// SPDX-License-Identifier: Apache-2.0
// Blackboard values and behaviour tree parsing/validation.
#include "victus/ai/ai.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace victus::ai {

// ---- values -----------------------------------------------------------------------------------

Value value_from_json(const json::Value& v) {
    switch (v.type()) {
    case json::Type::Bool: return v.as_bool();
    case json::Type::Number: return v.as_number();
    case json::Type::String: return v.as_string();
    case json::Type::Array:
        if (v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number())
            return math::Vec3{v[0].as_float(), v[1].as_float(), v[2].as_float()};
        return {};
    default: return {};
    }
}

std::string to_string(const Value& v) {
    struct Visitor {
        std::string operator()(std::monostate) const { return "nil"; }
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(double d) const { return std::format("{:g}", d); }
        std::string operator()(math::Vec3 p) const { return std::format("({:.2f}, {:.2f}, {:.2f})", p.x, p.y, p.z); }
        std::string operator()(Entity e) const {
            return e.is_null() ? std::string("entity(null)") : std::format("entity({}:{})", e.index, e.generation);
        }
        std::string operator()(const std::string& s) const { return s; }
    };
    return std::visit(Visitor{}, v);
}

bool truthy(const Value& v) noexcept {
    if (std::holds_alternative<std::monostate>(v)) return false;
    if (const bool* b = std::get_if<bool>(&v)) return *b;
    if (const double* d = std::get_if<double>(&v)) return *d != 0.0;
    if (const std::string* s = std::get_if<std::string>(&v)) return !s->empty();
    if (const Entity* e = std::get_if<Entity>(&v)) return !e->is_null();
    return true;
}

// ---- blackboard -------------------------------------------------------------------------------

namespace {
auto lower(std::vector<std::pair<std::string, Value>>& v, std::string_view key) {
    return std::lower_bound(v.begin(), v.end(), key, [](const auto& p, std::string_view k) { return p.first < k; });
}
auto lower(const std::vector<std::pair<std::string, Value>>& v, std::string_view key) {
    return std::lower_bound(v.begin(), v.end(), key, [](const auto& p, std::string_view k) { return p.first < k; });
}
} // namespace

void Blackboard::set(std::string_view key, Value value) {
    auto it = lower(entries_, key);
    if (it != entries_.end() && it->first == key) {
        if (it->second == value) return;
        it->second = std::move(value);
    } else {
        entries_.insert(it, {std::string(key), std::move(value)});
    }
    ++revision_;
}

bool Blackboard::erase(std::string_view key) {
    auto it = lower(entries_, key);
    if (it == entries_.end() || it->first != key) return false;
    entries_.erase(it);
    ++revision_;
    return true;
}

const Value* Blackboard::find(std::string_view key) const noexcept {
    auto it = lower(entries_, key);
    return it != entries_.end() && it->first == key ? &it->second : nullptr;
}

double Blackboard::number(std::string_view key, double fallback) const noexcept {
    const Value* v = find(key);
    if (!v) return fallback;
    if (const double* d = std::get_if<double>(v)) return *d;
    if (const bool* b = std::get_if<bool>(v)) return *b ? 1.0 : 0.0;
    return fallback;
}

bool Blackboard::boolean(std::string_view key, bool fallback) const noexcept {
    const Value* v = find(key);
    return v ? truthy(*v) : fallback;
}

std::optional<math::Vec3> Blackboard::vec3(std::string_view key) const noexcept {
    const Value* v = find(key);
    if (const math::Vec3* p = v ? std::get_if<math::Vec3>(v) : nullptr) return *p;
    return std::nullopt;
}

Entity Blackboard::entity(std::string_view key) const noexcept {
    const Value* v = find(key);
    if (const Entity* e = v ? std::get_if<Entity>(v) : nullptr) return *e;
    return {};
}

void Blackboard::clear() {
    if (entries_.empty()) return;
    entries_.clear();
    ++revision_;
}

// ---- names ------------------------------------------------------------------------------------

std::string_view status_name(NodeStatus s) noexcept {
    switch (s) {
    case NodeStatus::Idle: return "idle";
    case NodeStatus::Running: return "running";
    case NodeStatus::Success: return "success";
    case NodeStatus::Failure: return "failure";
    }
    return "?";
}

namespace {
struct TypeName {
    NodeType type;
    std::string_view name;
};
constexpr TypeName kTypes[] = {
    {NodeType::Sequence, "sequence"},   {NodeType::Selector, "selector"},     {NodeType::Parallel, "parallel"},
    {NodeType::Inverter, "inverter"},   {NodeType::Succeeder, "succeeder"},   {NodeType::Failer, "failer"},
    {NodeType::Repeat, "repeat"},       {NodeType::Cooldown, "cooldown"},     {NodeType::Timeout, "timeout"},
    {NodeType::Guard, "guard"},         {NodeType::Wait, "wait"},             {NodeType::Check, "check"},
    {NodeType::Set, "set"},             {NodeType::Clear, "clear"},           {NodeType::MoveTo, "move_to"},
    {NodeType::RandomPoint, "random_point"}, {NodeType::Action, "action"},   {NodeType::Succeed, "succeed"},
    {NodeType::Fail, "fail"},           {NodeType::Log, "log"},
};

struct OpName {
    CompareOp op;
    std::string_view name;
};
constexpr OpName kOps[] = {
    {CompareOp::IsSet, "set"},     {CompareOp::NotSet, "unset"},      {CompareOp::Equal, "=="},
    {CompareOp::NotEqual, "!="},   {CompareOp::Less, "<"},            {CompareOp::LessEqual, "<="},
    {CompareOp::Greater, ">"},     {CompareOp::GreaterEqual, ">="},   {CompareOp::True, "true"},
    {CompareOp::False, "false"},
};

using Arity = NodeArity;
Arity arity(NodeType t) { return node_arity(t); }

float finite_nonneg(const json::Value* v, float fallback) {
    if (!v || !v->is_number()) return fallback;
    const float f = v->as_float();
    return std::isfinite(f) ? std::max(f, 0.0f) : fallback;
}

struct Parser {
    std::vector<Node>& nodes;

    Result<std::uint32_t> node(const json::Value& v, std::uint32_t depth, const std::string& path) {
        if (!v.is_object()) return make_error(ErrorCode::ParseError, path + ": a node must be an object");
        if (nodes.size() >= BehaviorTree::kMaxNodes)
            return make_error(ErrorCode::LimitExceeded, std::format("more than {} nodes", BehaviorTree::kMaxNodes));
        const json::Value* type = v.find("type");
        if (!type || !type->is_string()) return make_error(ErrorCode::ParseError, path + ": missing \"type\"");
        const auto* t = std::find_if(std::begin(kTypes), std::end(kTypes), [&](const TypeName& n) { return n.name == type->as_string(); });
        if (t == std::end(kTypes)) return make_error(ErrorCode::ParseError, path + ": unknown node type \"" + type->as_string() + "\"");
        const auto index = static_cast<std::uint32_t>(nodes.size());
        {
            Node n;
            n.type = t->type;
            n.depth = depth;
            n.name = v["name"].as_string_or("");
            n.key = v["key"].as_string_or("");
            n.action = v["action"].as_string_or(v["message"].as_string_or(""));
            n.reactive = v["reactive"].as_bool(false);
            n.seconds = finite_nonneg(v.find("seconds"), 0.0f);
            n.deviation = finite_nonneg(v.find("deviation"), 0.0f);
            n.acceptance = finite_nonneg(v.find("acceptance"), 0.5f);
            n.speed = finite_nonneg(v.find("speed"), 3.5f);
            n.radius = finite_nonneg(v.find("radius"), 8.0f);
            n.count = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["count"].as_int(0), 0, 1 << 30));
            n.threshold = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["threshold"].as_int(0), 0, 1 << 30));
            if (const json::Value* val = v.find("value")) n.value = value_from_json(*val);
            if (const json::Value* op = v.find("op")) {
                if (!op->is_string()) return make_error(ErrorCode::ParseError, path + ": \"op\" must be a string");
                const auto* o = std::find_if(std::begin(kOps), std::end(kOps), [&](const OpName& x) { return x.name == op->as_string(); });
                if (o == std::end(kOps)) return make_error(ErrorCode::ParseError, path + ": unknown op \"" + op->as_string() + "\"");
                n.op = o->op;
            } else if (n.type == NodeType::Check || n.type == NodeType::Guard) {
                // A value means equality, no value means "is set".
                n.op = v.find("value") ? CompareOp::Equal : CompareOp::IsSet;
            }
            nodes.push_back(std::move(n));
        }
        const NodeType nt = t->type;
        const bool needs_key = nt == NodeType::Check || nt == NodeType::Guard || nt == NodeType::Set || nt == NodeType::Clear ||
                               nt == NodeType::MoveTo || nt == NodeType::RandomPoint;
        if (needs_key && nodes[index].key.empty()) return make_error(ErrorCode::ParseError, path + ": \"" + std::string(t->name) + "\" needs a \"key\"");
        if (nt == NodeType::Action && nodes[index].action.empty())
            return make_error(ErrorCode::ParseError, path + ": \"action\" needs an \"action\" name");
        if (nt == NodeType::Set && std::holds_alternative<std::monostate>(nodes[index].value))
            return make_error(ErrorCode::ParseError, path + ": \"set\" needs a \"value\" (number, bool, string or [x, y, z])");

        std::vector<const json::Value*> kids;
        if (const json::Value* c = v.find("children")) {
            if (!c->is_array()) return make_error(ErrorCode::ParseError, path + ": \"children\" must be an array");
            for (const auto& k : c->as_array()) kids.push_back(&k);
        }
        if (const json::Value* c = v.find("child")) kids.push_back(c);
        const Arity a = arity(nt);
        if (a == Arity::Leaf && !kids.empty()) return make_error(ErrorCode::ParseError, path + ": \"" + std::string(t->name) + "\" takes no children");
        if (a == Arity::One && kids.size() != 1) return make_error(ErrorCode::ParseError, path + ": \"" + std::string(t->name) + "\" needs exactly one child");
        if (a == Arity::Many && kids.empty()) return make_error(ErrorCode::ParseError, path + ": \"" + std::string(t->name) + "\" needs children");
        std::vector<std::uint32_t> children;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            auto c = node(*kids[i], depth + 1, std::format("{}/{}", path, i));
            if (!c) return std::move(c).error();
            children.push_back(*c);
        }
        nodes[index].children = std::move(children);
        if (nt == NodeType::Parallel && nodes[index].threshold > nodes[index].children.size())
            nodes[index].threshold = static_cast<std::uint32_t>(nodes[index].children.size());
        return index;
    }
};
} // namespace

NodeArity node_arity(NodeType t) noexcept {
    switch (t) {
    case NodeType::Sequence:
    case NodeType::Selector:
    case NodeType::Parallel: return NodeArity::Many;
    case NodeType::Inverter:
    case NodeType::Succeeder:
    case NodeType::Failer:
    case NodeType::Repeat:
    case NodeType::Cooldown:
    case NodeType::Timeout:
    case NodeType::Guard: return NodeArity::One;
    default: return NodeArity::Leaf;
    }
}

std::optional<NodeType> node_type_from_name(std::string_view name) noexcept {
    for (const auto& n : kTypes)
        if (n.name == name) return n.type;
    return std::nullopt;
}

std::string_view compare_op_name(CompareOp op) noexcept {
    for (const auto& o : kOps)
        if (o.op == op) return o.name;
    return "?";
}

json::Value value_to_json(const Value& v) {
    struct Visitor {
        json::Value operator()(std::monostate) const { return {}; }
        json::Value operator()(bool b) const { return b; }
        json::Value operator()(double d) const { return d; }
        json::Value operator()(math::Vec3 p) const { return json::Array{json::Value(p.x), json::Value(p.y), json::Value(p.z)}; }
        json::Value operator()(Entity) const { return {}; }
        json::Value operator()(const std::string& s) const { return s; }
    };
    return std::visit(Visitor{}, v);
}

namespace {
Result<json::Value> node_to_json(const std::vector<Node>& nodes, std::uint32_t index, std::vector<std::uint8_t>& seen, std::uint32_t depth) {
    if (index >= nodes.size()) return make_error(ErrorCode::InvalidArgument, std::format("child index {} is out of range", index));
    if (seen[index]) return make_error(ErrorCode::InvalidArgument, std::format("node {} is used twice (or in a cycle)", index));
    if (depth > 120) return make_error(ErrorCode::LimitExceeded, "the tree is more than 120 levels deep");
    seen[index] = 1;
    const Node& n = nodes[index];
    const Node def;
    json::Value o = json::Value::object();
    o.set("type", std::string(node_type_name(n.type)));
    if (!n.name.empty()) o.set("name", n.name);
    if (!n.key.empty()) o.set("key", n.key);
    const bool compares = n.type == NodeType::Check || n.type == NodeType::Guard;
    if (compares) o.set("op", std::string(compare_op_name(n.op)));
    if (!std::holds_alternative<std::monostate>(n.value)) {
        if (json::Value v = value_to_json(n.value); !v.is_null()) o.set("value", std::move(v));
    }
    if (n.type == NodeType::Action && !n.action.empty()) o.set("action", n.action);
    if (n.type == NodeType::Log && !n.action.empty()) o.set("message", n.action);
    if (n.seconds != def.seconds) o.set("seconds", n.seconds);
    if (n.deviation != def.deviation) o.set("deviation", n.deviation);
    if (n.count != def.count) o.set("count", n.count);
    if (n.threshold != def.threshold) o.set("threshold", n.threshold);
    if (n.reactive) o.set("reactive", true);
    if (n.type == NodeType::MoveTo) {
        if (n.acceptance != def.acceptance) o.set("acceptance", n.acceptance);
        if (n.speed != def.speed) o.set("speed", n.speed);
    }
    if (n.type == NodeType::RandomPoint && n.radius != def.radius) o.set("radius", n.radius);
    if (!n.children.empty()) {
        json::Value kids = json::Value::array();
        for (const std::uint32_t c : n.children) {
            auto k = node_to_json(nodes, c, seen, depth + 1);
            if (!k) return k;
            kids.push(std::move(*k));
        }
        o.set("children", std::move(kids));
    }
    return o;
}
} // namespace

Result<json::Value> tree_to_json(const std::vector<Node>& nodes, const Blackboard& defaults) {
    if (nodes.empty()) return make_error(ErrorCode::InvalidArgument, "the tree has no nodes");
    std::vector<std::uint8_t> seen(nodes.size(), 0);
    auto root = node_to_json(nodes, 0, seen, 0);
    if (!root) return root;
    json::Value doc = json::Value::object();
    doc.set("root", std::move(*root));
    if (!defaults.entries().empty()) {
        json::Value bb = json::Value::object();
        for (const auto& [k, v] : defaults.entries())
            if (json::Value j = value_to_json(v); !j.is_null()) bb.set(k, std::move(j));
        doc.set("blackboard", std::move(bb));
    }
    return doc;
}

std::string_view node_type_name(NodeType t) noexcept {
    for (const auto& n : kTypes)
        if (n.type == t) return n.name;
    return "?";
}

Result<std::shared_ptr<const BehaviorTree>> BehaviorTree::from_json(const json::Value& doc, std::string name) {
    if (!doc.is_object()) return make_error(ErrorCode::ParseError, "behaviour tree: expected an object");
    const json::Value* root = doc.find("root");
    if (!root) return make_error(ErrorCode::ParseError, "behaviour tree: missing \"root\"");
    auto tree = std::make_shared<BehaviorTree>();
    tree->name_ = std::move(name);
    Parser p{tree->nodes_};
    auto r = p.node(*root, 0, "root");
    if (!r) return make_error(r.error().code, "behaviour tree: " + r.error().message);
    if (const json::Value* bb = doc.find("blackboard")) {
        if (!bb->is_object()) return make_error(ErrorCode::ParseError, "behaviour tree: \"blackboard\" must be an object");
        for (const auto& [k, v] : bb->as_object()) tree->defaults_.set(k, value_from_json(v));
    }
    return std::shared_ptr<const BehaviorTree>(std::move(tree));
}

Result<std::shared_ptr<const BehaviorTree>> BehaviorTree::parse(std::string_view text, std::string name) {
    auto doc = json::parse(text, {.max_depth = 128, .max_bytes = 16u * 1024u * 1024u, .max_string_bytes = 1u << 20});
    if (!doc) return make_error(ErrorCode::ParseError, "behaviour tree: " + doc.error().message);
    return from_json(*doc, std::move(name));
}

} // namespace victus::ai
