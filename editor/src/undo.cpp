// SPDX-License-Identifier: Apache-2.0
#include "victus/editor/undo.hpp"

namespace victus::editor {

namespace {
Uuid uuid_of(const json::Value& v) {
    Uuid u;
    if (!Uuid::parse(v.as_string_or(""), u)) return Uuid{};
    return u;
}
} // namespace

Status apply_states(scene::Scene& scene, const std::vector<EntityState>& states) {
    for (auto it = states.rbegin(); it != states.rend(); ++it) {
        if (it->snapshot) continue;
        if (const auto e = scene.find(it->uuid); !e.is_null()) scene.destroy(e);
    }
    for (const auto& s : states) {
        if (!s.snapshot) continue;
        auto r = scene.restore_entity(*s.snapshot);
        if (!r) return std::move(r).error();
    }
    // Second pass fixes sibling order for entities whose "next" sibling was restored later.
    for (const auto& s : states) {
        if (!s.snapshot) continue;
        if (const json::Value* nv = s.snapshot->find("next"); nv && nv->is_string()) {
            const Uuid next = uuid_of(*nv);
            const auto e = scene.find(s.uuid);
            const auto n = scene.find(next);
            if (!e.is_null() && !n.is_null() && scene.next_sibling(e) != n && scene.parent(n) == scene.parent(e))
                scene.move_before(e, n);
        }
    }
    scene.update_transforms();
    return {};
}

std::vector<EntityState> capture(const scene::Scene& scene, const std::vector<Uuid>& entities) {
    std::vector<EntityState> out;
    for (const Uuid& id : entities) {
        const auto e = scene.find(id);
        EntityState st{id, std::nullopt};
        if (!e.is_null()) {
            json::Value snap = scene.save_entity(e);
            if (const auto n = scene.next_sibling(e); !n.is_null()) snap.set("next", scene.uuid(n).to_string());
            st.snapshot = std::move(snap);
        }
        out.push_back(std::move(st));
    }
    return out;
}

std::vector<EntityState> capture_subtrees(const scene::Scene& scene, const std::vector<Uuid>& roots) {
    std::vector<EntityState> out;
    for (const Uuid& id : roots) {
        const auto e = scene.find(id);
        if (e.is_null()) continue;
        const json::Value arr = scene.save_subtree(e);
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const Uuid u = uuid_of(arr[i]["uuid"]);
            out.push_back({u, arr[i]});
        }
    }
    return out;
}

std::vector<EntityState> absent(const std::vector<EntityState>& like) {
    std::vector<EntityState> out;
    out.reserve(like.size());
    for (const auto& s : like) out.push_back({s.uuid, std::nullopt});
    return out;
}

void UndoStack::push(SceneCommand command) {
    commands_.resize(position_);
    ids_.resize(position_);
    commands_.push_back(std::move(command));
    ids_.push_back(next_id_++);
    if (commands_.size() > limit_) {
        commands_.erase(commands_.begin());
        ids_.erase(ids_.begin());
    }
    position_ = commands_.size();
}

Status UndoStack::undo(scene::Scene& scene) {
    if (!can_undo()) return {};
    const SceneCommand& c = commands_[position_ - 1];
    VICTUS_TRY_STATUS(apply_states(scene, c.before));
    --position_;
    return {};
}

Status UndoStack::redo(scene::Scene& scene) {
    if (!can_redo()) return {};
    const SceneCommand& c = commands_[position_];
    VICTUS_TRY_STATUS(apply_states(scene, c.after));
    ++position_;
    return {};
}

void UndoStack::clear() noexcept {
    commands_.clear();
    ids_.clear();
    position_ = 0;
    saved_ = 0;
}

} // namespace victus::editor
