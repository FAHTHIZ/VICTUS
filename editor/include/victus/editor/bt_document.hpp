// SPDX-License-Identifier: Apache-2.0
// The editable model behind the Behaviour Tree Editor (ADR-0176): one .vbt file as a node list in
// pre-order (node 0 is the root), with structural edits that keep the tree valid-shaped, a
// snapshot undo history, per-node problems in plain words, and save/load through the runtime's
// own parser and serializer so what the editor shows is exactly what the game runs.
#pragma once

#include "victus/ai/ai.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::editor {

// Display metadata for the node palette and the canvas.
struct BtNodeInfo {
    ai::NodeType type;
    const char* label;       // "Sequence", "Move To"
    const char* category;    // "Composite", "Decorator", "Condition", "Task"
    const char* description; // one plain sentence
};
[[nodiscard]] const std::vector<BtNodeInfo>& bt_node_catalog();
[[nodiscard]] const BtNodeInfo& bt_node_info(ai::NodeType t);
// A node with sensible defaults for its type (keys, durations), so a fresh node is valid.
[[nodiscard]] ai::Node bt_default_node(ai::NodeType t);
// One-line summary of a node's settings: "is target set?", "wait 2 s (±0.5)".
[[nodiscard]] std::string bt_node_summary(const ai::Node& n);

class BehaviorTreeDocument {
public:
    static constexpr std::size_t kMaxUndo = 200;

    // A new, valid tree: a Selector root with a Wait child.
    BehaviorTreeDocument();

    [[nodiscard]] static Result<BehaviorTreeDocument> load(const std::filesystem::path& file, std::string asset_path);
    [[nodiscard]] static Result<BehaviorTreeDocument> from_text(std::string_view text, std::string asset_path = {});
    // Writes the file (the runtime hot-reloads it on the next tree load) and clears dirty.
    Status save();
    Status save_as(const std::filesystem::path& file, std::string asset_path);
    [[nodiscard]] Result<std::string> to_text() const;

    [[nodiscard]] const std::vector<ai::Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const ai::Blackboard& defaults() const noexcept { return defaults_; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    [[nodiscard]] const std::string& asset_path() const noexcept { return asset_path_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] int parent_of(int node) const noexcept;
    [[nodiscard]] bool can_add_child(int node) const noexcept;

    // Structural edits. Each records one undo step and returns the index of the node to select
    // afterwards (indices change: the list is kept in pre-order), or an error with the reason.
    Result<int> add_child(int parent, ai::NodeType type);
    Result<int> wrap(int node, ai::NodeType decorator); // insert a decorator above `node`
    Result<int> remove(int node);                       // node and its subtree; returns the parent
    Result<int> move(int node, int delta);              // reorder among siblings
    Result<int> change_type(int node, ai::NodeType type);
    Result<int> duplicate(int node);                    // copy of the subtree after the original
    // Moves `node` and its subtree under `new_parent` at child position `index` (-1 appends;
    // positions count the new parent's children without `node`). Works within one parent too.
    Result<int> reparent(int node, int new_parent, int index = -1);
    // Why reparent(node, new_parent) would be refused, in plain words; empty when allowed.
    [[nodiscard]] std::string reparent_problem(int node, int new_parent) const;
    [[nodiscard]] bool is_descendant(int node, int ancestor) const noexcept; // node inside ancestor's subtree

    // Property edits: call begin_edit() before changing the node (once per gesture), then
    // mutable_node(); end_edit() marks the document dirty.
    void begin_edit(std::string_view label);
    [[nodiscard]] ai::Node& mutable_node(int node) { return nodes_.at(static_cast<std::size_t>(node)); }
    [[nodiscard]] ai::Blackboard& mutable_defaults() noexcept { return defaults_; }
    void end_edit() noexcept { dirty_ = true; }

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::string undo_label() const { return undo_.empty() ? std::string() : undo_.back().label; }
    bool undo();
    bool redo();

    // Plain-words problem with one node ("needs at least one child"), empty when fine.
    [[nodiscard]] std::string node_problem(int node) const;
    // First problem in the tree, or the runtime parser's error; empty when the game can run it.
    [[nodiscard]] std::string validate() const;

private:
    struct Snapshot {
        std::string label;
        std::vector<ai::Node> nodes;
        ai::Blackboard defaults;
    };
    void push_undo(std::string label);
    // Rebuilds pre-order from the root (dropping unreachable nodes) and returns where `track`
    // ended up (-1 when dropped).
    int normalize(int track);

    std::vector<ai::Node> nodes_;
    ai::Blackboard defaults_;
    std::filesystem::path file_;
    std::string asset_path_;
    bool dirty_{false};
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
};

} // namespace victus::editor
