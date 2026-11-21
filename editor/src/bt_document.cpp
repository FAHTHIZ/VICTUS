// SPDX-License-Identifier: Apache-2.0
// Behaviour Tree Editor model (ADR-0176). See bt_document.hpp.
#include "victus/editor/bt_document.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"

#include <algorithm>
#include <format>
#include <functional>

namespace victus::editor {

using ai::Node;
using ai::NodeArity;
using ai::NodeType;

namespace {

bool needs_key(NodeType t) {
    return t == NodeType::Check || t == NodeType::Guard || t == NodeType::Set || t == NodeType::Clear ||
           t == NodeType::MoveTo || t == NodeType::RandomPoint;
}

std::string value_words(const ai::Value& v) {
    if (const auto* s = std::get_if<std::string>(&v)) return "\"" + *s + "\"";
    return ai::to_string(v);
}

std::string op_words(const Node& n) {
    const std::string& k = n.key;
    switch (n.op) {
    case ai::CompareOp::IsSet: return k + " is set";
    case ai::CompareOp::NotSet: return k + " is not set";
    case ai::CompareOp::True: return k + " is true";
    case ai::CompareOp::False: return k + " is false";
    default: break;
    }
    return k + " " + std::string(ai::compare_op_name(n.op)) + " " + value_words(n.value);
}

} // namespace

const std::vector<BtNodeInfo>& bt_node_catalog() {
    static const std::vector<BtNodeInfo> catalog = {
        {NodeType::Sequence, "Sequence", "Composite", "Runs children left to right; stops at the first one that fails."},
        {NodeType::Selector, "Selector", "Composite", "Tries children left to right; stops at the first one that succeeds."},
        {NodeType::Parallel, "Parallel", "Composite", "Runs all children at once; succeeds when enough of them succeed."},
        {NodeType::Inverter, "Inverter", "Decorator", "Turns success into failure and failure into success."},
        {NodeType::Succeeder, "Always Succeed", "Decorator", "Runs the child and reports success whatever happens."},
        {NodeType::Failer, "Always Fail", "Decorator", "Runs the child and reports failure whatever happens."},
        {NodeType::Repeat, "Repeat", "Decorator", "Runs the child again and again (a number of times, or forever)."},
        {NodeType::Cooldown, "Cooldown", "Decorator", "After the child finishes, fails for a while before it may run again."},
        {NodeType::Timeout, "Timeout", "Decorator", "Fails the child if it is still running after a time limit."},
        {NodeType::Guard, "Guard", "Decorator", "Only runs the child while a blackboard condition holds."},
        {NodeType::Check, "Check", "Condition", "Succeeds when a blackboard value matches a condition."},
        {NodeType::Wait, "Wait", "Task", "Waits a number of seconds, then succeeds."},
        {NodeType::Set, "Set Value", "Task", "Writes a value to the blackboard."},
        {NodeType::Clear, "Clear Value", "Task", "Removes a value from the blackboard."},
        {NodeType::MoveTo, "Move To", "Task", "Walks to a position or entity stored in the blackboard."},
        {NodeType::RandomPoint, "Random Point", "Task", "Picks a random reachable point nearby and stores it."},
        {NodeType::Action, "Action", "Task", "Runs a method of the entity's script (or a C++ action) by name."},
        {NodeType::Log, "Log", "Task", "Prints a message to the Console."},
        {NodeType::Succeed, "Succeed", "Task", "Succeeds immediately."},
        {NodeType::Fail, "Fail", "Task", "Fails immediately."},
    };
    return catalog;
}

const BtNodeInfo& bt_node_info(NodeType t) {
    const auto& c = bt_node_catalog();
    const auto it = std::find_if(c.begin(), c.end(), [&](const BtNodeInfo& i) { return i.type == t; });
    return it != c.end() ? *it : c.back();
}

Node bt_default_node(NodeType t) {
    Node n;
    n.type = t;
    switch (t) {
    case NodeType::Check:
    case NodeType::Guard:
    case NodeType::MoveTo: n.key = "target"; break;
    case NodeType::Clear: n.key = "target"; break;
    case NodeType::Set:
        n.key = "alert";
        n.value = true;
        break;
    case NodeType::RandomPoint: n.key = "destination"; break;
    case NodeType::Wait: n.seconds = 1.0f; break;
    case NodeType::Cooldown: n.seconds = 2.0f; break;
    case NodeType::Timeout: n.seconds = 5.0f; break;
    case NodeType::Action: n.action = "my_action"; break;
    case NodeType::Log: n.action = "Hello from the behaviour tree"; break;
    default: break;
    }
    return n;
}

std::string bt_node_summary(const Node& n) {
    switch (n.type) {
    case NodeType::Sequence:
    case NodeType::Selector: return n.reactive ? "re-checks earlier children" : std::string();
    case NodeType::Parallel:
        return n.threshold == 0 ? "needs all to succeed" : std::format("needs {} to succeed", n.threshold);
    case NodeType::Repeat: return n.count == 0 ? "forever" : std::format("{} times", n.count);
    case NodeType::Cooldown: return std::format("{:g} s", n.seconds);
    case NodeType::Timeout: return std::format("limit {:g} s", n.seconds);
    case NodeType::Guard:
    case NodeType::Check: return op_words(n);
    case NodeType::Wait:
        return n.deviation > 0 ? std::format("{:g} s (±{:g})", n.seconds, n.deviation) : std::format("{:g} s", n.seconds);
    case NodeType::Set: return n.key + " = " + value_words(n.value);
    case NodeType::Clear: return "clear " + n.key;
    case NodeType::MoveTo: return "to " + n.key;
    case NodeType::RandomPoint: return std::format("{} within {:g} m", n.key, n.radius);
    case NodeType::Action: return n.action;
    case NodeType::Log: return "\"" + n.action + "\"";
    default: return {};
    }
}

// ---- document -----------------------------------------------------------------------------------

BehaviorTreeDocument::BehaviorTreeDocument() {
    Node root = bt_default_node(NodeType::Selector);
    root.children = {1};
    Node wait = bt_default_node(NodeType::Wait);
    wait.depth = 1;
    nodes_ = {std::move(root), std::move(wait)};
}

Result<BehaviorTreeDocument> BehaviorTreeDocument::from_text(std::string_view text, std::string asset_path) {
    auto tree = ai::BehaviorTree::parse(text, asset_path);
    if (!tree) return std::move(tree).error();
    BehaviorTreeDocument doc;
    doc.nodes_ = (*tree)->nodes();
    doc.defaults_ = (*tree)->defaults();
    doc.asset_path_ = std::move(asset_path);
    doc.normalize(0);
    return doc;
}

Result<BehaviorTreeDocument> BehaviorTreeDocument::load(const std::filesystem::path& file, std::string asset_path) {
    auto text = fs::read_text(file, 16u << 20);
    if (!text) return std::move(text).error();
    auto doc = from_text(*text, std::move(asset_path));
    if (!doc) return std::move(doc).error();
    doc->file_ = file;
    return doc;
}

Result<std::string> BehaviorTreeDocument::to_text() const {
    auto j = ai::tree_to_json(nodes_, defaults_);
    if (!j) return std::move(j).error();
    return json::stringify(*j, true) + "\n";
}

Status BehaviorTreeDocument::save() {
    if (file_.empty()) return make_error(ErrorCode::InvalidArgument, "this behaviour tree has no file yet");
    auto text = to_text();
    if (!text) return std::move(text).error();
    if (auto st = fs::write_text_atomic(file_, *text); !st.ok()) return st;
    dirty_ = false;
    return {};
}

Status BehaviorTreeDocument::save_as(const std::filesystem::path& file, std::string asset_path) {
    file_ = file;
    asset_path_ = std::move(asset_path);
    return save();
}

int BehaviorTreeDocument::parent_of(int node) const noexcept {
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        for (const std::uint32_t c : nodes_[i].children)
            if (static_cast<int>(c) == node) return static_cast<int>(i);
    return -1;
}

bool BehaviorTreeDocument::can_add_child(int node) const noexcept {
    if (node < 0 || node >= static_cast<int>(nodes_.size())) return false;
    const Node& n = nodes_[static_cast<std::size_t>(node)];
    switch (ai::node_arity(n.type)) {
    case NodeArity::Many: return true;
    case NodeArity::One: return n.children.empty();
    case NodeArity::Leaf: return false;
    }
    return false;
}

void BehaviorTreeDocument::push_undo(std::string label) {
    undo_.push_back({std::move(label), nodes_, defaults_});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void BehaviorTreeDocument::begin_edit(std::string_view label) { push_undo(std::string(label)); }

bool BehaviorTreeDocument::undo() {
    if (undo_.empty()) return false;
    redo_.push_back({undo_.back().label, nodes_, defaults_});
    nodes_ = std::move(undo_.back().nodes);
    defaults_ = std::move(undo_.back().defaults);
    undo_.pop_back();
    dirty_ = true;
    return true;
}

bool BehaviorTreeDocument::redo() {
    if (redo_.empty()) return false;
    undo_.push_back({redo_.back().label, nodes_, defaults_});
    nodes_ = std::move(redo_.back().nodes);
    defaults_ = std::move(redo_.back().defaults);
    redo_.pop_back();
    dirty_ = true;
    return true;
}

int BehaviorTreeDocument::normalize(int track) {
    std::vector<Node> out;
    out.reserve(nodes_.size());
    std::vector<int> remap(nodes_.size(), -1);
    std::function<std::uint32_t(std::uint32_t, std::uint32_t)> visit = [&](std::uint32_t i, std::uint32_t depth) {
        const auto me = static_cast<std::uint32_t>(out.size());
        remap[i] = static_cast<int>(me);
        out.push_back(nodes_[i]);
        out[me].depth = depth;
        std::vector<std::uint32_t> kids;
        for (const std::uint32_t c : nodes_[i].children)
            if (c < nodes_.size() && remap[c] < 0) kids.push_back(visit(c, depth + 1));
        out[me].children = std::move(kids);
        return me;
    };
    if (!nodes_.empty()) visit(0, 0);
    nodes_ = std::move(out);
    return track >= 0 && track < static_cast<int>(remap.size()) ? remap[static_cast<std::size_t>(track)] : -1;
}

Result<int> BehaviorTreeDocument::add_child(int parent, NodeType type) {
    if (parent < 0 || parent >= static_cast<int>(nodes_.size())) return make_error(ErrorCode::InvalidArgument, "select a node first");
    const Node& p = nodes_[static_cast<std::size_t>(parent)];
    if (!can_add_child(parent)) {
        const char* why = ai::node_arity(p.type) == NodeArity::Leaf ? "cannot have children" : "already has its one child";
        return make_error(ErrorCode::InvalidArgument, std::format("{} {}", bt_node_info(p.type).label, why));
    }
    if (nodes_.size() >= ai::BehaviorTree::kMaxNodes) return make_error(ErrorCode::LimitExceeded, "the tree is full");
    push_undo(std::format("Add {}", bt_node_info(type).label));
    const auto added = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(bt_default_node(type));
    nodes_[static_cast<std::size_t>(parent)].children.push_back(added);
    dirty_ = true;
    return normalize(static_cast<int>(added));
}

Result<int> BehaviorTreeDocument::wrap(int node, NodeType decorator) {
    if (node < 0 || node >= static_cast<int>(nodes_.size())) return make_error(ErrorCode::InvalidArgument, "select a node first");
    if (ai::node_arity(decorator) != NodeArity::One)
        return make_error(ErrorCode::InvalidArgument, std::format("{} is not a decorator", bt_node_info(decorator).label));
    push_undo(std::format("Wrap in {}", bt_node_info(decorator).label));
    const auto added = static_cast<std::uint32_t>(nodes_.size());
    Node d = bt_default_node(decorator);
    if (node == 0) {
        // The new decorator becomes the root: swap it into slot 0.
        nodes_.push_back(std::move(nodes_[0]));
        d.children = {added};
        nodes_[0] = std::move(d);
        dirty_ = true;
        return normalize(0);
    }
    const int parent = parent_of(node);
    d.children = {static_cast<std::uint32_t>(node)};
    nodes_.push_back(std::move(d));
    for (auto& c : nodes_[static_cast<std::size_t>(parent)].children)
        if (static_cast<int>(c) == node) c = added;
    dirty_ = true;
    return normalize(static_cast<int>(added));
}

Result<int> BehaviorTreeDocument::remove(int node) {
    if (node <= 0 || node >= static_cast<int>(nodes_.size()))
        return make_error(ErrorCode::InvalidArgument, node == 0 ? "the root cannot be deleted; change its type instead" : "select a node first");
    const int parent = parent_of(node);
    push_undo(std::format("Delete {}", bt_node_info(nodes_[static_cast<std::size_t>(node)].type).label));
    auto& kids = nodes_[static_cast<std::size_t>(parent)].children;
    kids.erase(std::remove(kids.begin(), kids.end(), static_cast<std::uint32_t>(node)), kids.end());
    dirty_ = true;
    return normalize(parent);
}

Result<int> BehaviorTreeDocument::move(int node, int delta) {
    const int parent = parent_of(node);
    if (parent < 0) return make_error(ErrorCode::InvalidArgument, "the root has no siblings");
    auto& kids = nodes_[static_cast<std::size_t>(parent)].children;
    const auto it = std::find(kids.begin(), kids.end(), static_cast<std::uint32_t>(node));
    const std::ptrdiff_t pos = it - kids.begin();
    const std::ptrdiff_t to = pos + delta;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(kids.size())) return make_error(ErrorCode::InvalidArgument, "already at the end");
    push_undo(delta < 0 ? "Move Left" : "Move Right");
    auto& k = nodes_[static_cast<std::size_t>(parent)].children;
    std::swap(k[static_cast<std::size_t>(pos)], k[static_cast<std::size_t>(to)]);
    dirty_ = true;
    return normalize(node);
}

Result<int> BehaviorTreeDocument::change_type(int node, NodeType type) {
    if (node < 0 || node >= static_cast<int>(nodes_.size())) return make_error(ErrorCode::InvalidArgument, "select a node first");
    Node& n = nodes_[static_cast<std::size_t>(node)];
    const std::size_t kids = n.children.size();
    const NodeArity a = ai::node_arity(type);
    if ((a == NodeArity::Leaf && kids > 0) || (a == NodeArity::One && kids > 1))
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{} can hold {} child nodes; remove some first", bt_node_info(type).label,
                                      a == NodeArity::Leaf ? "no" : "only one"));
    push_undo(std::format("Change to {}", bt_node_info(type).label));
    Node fresh = bt_default_node(type);
    fresh.children = std::move(n.children);
    fresh.name = std::move(n.name);
    fresh.depth = n.depth;
    // Keep the key/value when both types use them (Check -> Guard keeps its condition).
    if (needs_key(type) && !n.key.empty()) fresh.key = n.key;
    if ((type == NodeType::Check || type == NodeType::Guard) && (n.type == NodeType::Check || n.type == NodeType::Guard)) {
        fresh.op = n.op;
        fresh.value = n.value;
    }
    n = std::move(fresh);
    dirty_ = true;
    return node;
}

Result<int> BehaviorTreeDocument::duplicate(int node) {
    const int parent = parent_of(node);
    if (parent < 0) return make_error(ErrorCode::InvalidArgument, "the root cannot be duplicated");
    if (!can_add_child(parent))
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{} holds only one child", bt_node_info(nodes_[static_cast<std::size_t>(parent)].type).label));
    push_undo("Duplicate");
    std::function<std::uint32_t(std::uint32_t)> copy = [&](std::uint32_t i) {
        Node n = nodes_[i];
        std::vector<std::uint32_t> kids;
        for (const std::uint32_t c : n.children) kids.push_back(copy(c));
        n.children = std::move(kids);
        nodes_.push_back(std::move(n));
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    };
    const std::uint32_t added = copy(static_cast<std::uint32_t>(node));
    auto& kids = nodes_[static_cast<std::size_t>(parent)].children;
    const auto it = std::find(kids.begin(), kids.end(), static_cast<std::uint32_t>(node));
    kids.insert(it + 1, added);
    dirty_ = true;
    return normalize(static_cast<int>(added));
}

std::string BehaviorTreeDocument::node_problem(int node) const {
    if (node < 0 || node >= static_cast<int>(nodes_.size())) return {};
    const Node& n = nodes_[static_cast<std::size_t>(node)];
    switch (ai::node_arity(n.type)) {
    case NodeArity::Many:
        if (n.children.empty()) return "needs at least one child";
        break;
    case NodeArity::One:
        if (n.children.size() != 1) return "needs one child";
        break;
    case NodeArity::Leaf: break;
    }
    if (needs_key(n.type) && n.key.empty()) return "needs a blackboard key";
    if (n.type == NodeType::Action && n.action.empty()) return "needs an action name";
    if (n.type == NodeType::Set && std::holds_alternative<std::monostate>(n.value)) return "needs a value to write";
    if (n.type == NodeType::Parallel && n.threshold > n.children.size())
        return std::format("needs {} successes but has only {} children", n.threshold, n.children.size());
    return {};
}

std::string BehaviorTreeDocument::validate() const {
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
        if (auto p = node_problem(i); !p.empty())
            return std::format("{}: {}", bt_node_info(nodes_[static_cast<std::size_t>(i)].type).label, p);
    auto text = to_text();
    if (!text) return text.error().message;
    if (auto tree = ai::BehaviorTree::parse(*text); !tree) return tree.error().message;
    return {};
}

} // namespace victus::editor
