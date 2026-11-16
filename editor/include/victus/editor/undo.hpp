// SPDX-License-Identifier: Apache-2.0
// Undo/redo for scene edits. Every command is a set of per-entity state transitions
// (snapshot → snapshot, absent → snapshot, snapshot → absent), so one mechanism covers
// property edits, creation, deletion, duplication and reparenting, and stays valid when entity
// handles change (entities are addressed by UUID).
#pragma once
#include "victus/scene/scene.hpp"

#include <optional>
#include <string>
#include <vector>

namespace victus::editor {

struct EntityState {
    Uuid uuid;
    std::optional<json::Value> snapshot; // nullopt: entity absent
};

struct SceneCommand {
    std::string label;
    std::vector<EntityState> before; // parents before children
    std::vector<EntityState> after;
};

// Applies states: removes entities that must be absent, then restores present ones in order.
[[nodiscard]] Status apply_states(scene::Scene& scene, const std::vector<EntityState>& states);

// Helpers to build states.
[[nodiscard]] std::vector<EntityState> capture(const scene::Scene& scene, const std::vector<Uuid>& entities);
[[nodiscard]] std::vector<EntityState> capture_subtrees(const scene::Scene& scene, const std::vector<Uuid>& roots);
[[nodiscard]] std::vector<EntityState> absent(const std::vector<EntityState>& like);

class UndoStack {
public:
    explicit UndoStack(std::size_t limit = 256) : limit_(limit) {}
    // Records an already-applied command.
    void push(SceneCommand command);
    [[nodiscard]] bool can_undo() const noexcept { return position_ > 0; }
    [[nodiscard]] bool can_redo() const noexcept { return position_ < commands_.size(); }
    [[nodiscard]] Status undo(scene::Scene& scene);
    [[nodiscard]] Status redo(scene::Scene& scene);
    [[nodiscard]] const SceneCommand* next_undo() const noexcept { return can_undo() ? &commands_[position_ - 1] : nullptr; }
    [[nodiscard]] const SceneCommand* next_redo() const noexcept { return can_redo() ? &commands_[position_] : nullptr; }
    void clear() noexcept;
    // Saved-state tracking for the "unsaved changes" indicator.
    void mark_saved() noexcept { saved_ = static_cast<std::int64_t>(revision()); }
    [[nodiscard]] bool dirty() const noexcept { return saved_ != static_cast<std::int64_t>(revision()); }
    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

private:
    // A monotonically increasing id per command position so undo-then-new-edit is dirty.
    [[nodiscard]] std::uint64_t revision() const noexcept { return position_ ? ids_[position_ - 1] : 0; }
    std::vector<SceneCommand> commands_;
    std::vector<std::uint64_t> ids_;
    std::size_t position_{0};
    std::size_t limit_;
    std::uint64_t next_id_{1};
    std::int64_t saved_{0};
};

} // namespace victus::editor
