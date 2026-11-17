// SPDX-License-Identifier: Apache-2.0
// Multi-selection: viewport box select, hierarchy range select and multi-entity inspector edits
// (a change made on the primary selection is applied to every selected entity, field by field).
#include "victus/editor/editor.hpp"

#include "victus/scene/prefab.hpp"

#include <algorithm>
#include <unordered_set>

namespace victus::editor {

using namespace scene;

namespace {

void dfs(const Scene& s, Entity e, std::vector<Entity>& out) {
    out.push_back(e);
    for (const Entity c : s.children(e)) dfs(s, c, out);
}

bool erase_key(json::Value& obj, std::string_view key) {
    auto& o = obj.as_object();
    const auto it = std::find_if(o.begin(), o.end(), [&](const auto& kv) { return kv.first == key; });
    if (it == o.end()) return false;
    o.erase(it);
    return true;
}

} // namespace

json::Value merge_edit(const json::Value& before, const json::Value& after, const json::Value& target) {
    if (before == after) return target;
    if (before.is_object() && after.is_object() && target.is_object()) {
        json::Value out = target;
        for (const auto& [key, a] : after.as_object()) {
            const json::Value* b = before.find(key);
            if (!b) {
                out.set(key, a); // added on the primary (e.g. a component)
                continue;
            }
            if (*b == a) continue;
            if (const json::Value* t = target.find(key)) out.set(key, merge_edit(*b, a, *t));
            // A field the target does not have (other component kind/version) is left alone.
        }
        for (const auto& [key, b] : before.as_object()) {
            (void)b;
            if (!after.find(key)) erase_key(out, key); // removed on the primary
        }
        return out;
    }
    if (before.is_array() && after.is_array() && target.is_array() && before.size() == after.size() &&
        target.size() == after.size()) {
        // Vectors and colours: only changed elements (e.g. position.y) are applied.
        json::Value out = json::Value::array();
        for (std::size_t i = 0; i < after.size(); ++i) out.push(merge_edit(before[i], after[i], target[i]));
        return out;
    }
    return after;
}

std::vector<Entity> Editor::hierarchy_order() {
    Scene& s = active_scene();
    std::vector<Entity> out;
    for (const Entity r : s.roots()) dfs(s, r, out);
    return out;
}

void Editor::select_range(Entity to) {
    Scene& s = active_scene();
    if (to.is_null()) return;
    const Entity anchor = selection_.empty() ? kNullEntity : s.find(selection_.back());
    if (anchor.is_null()) {
        select(to);
        return;
    }
    const auto order = hierarchy_order();
    const auto ia = std::find(order.begin(), order.end(), anchor);
    const auto ib = std::find(order.begin(), order.end(), to);
    if (ia == order.end() || ib == order.end()) {
        select(to);
        return;
    }
    const auto [lo, hi] = std::minmax(ia, ib);
    for (auto it = lo; it <= hi; ++it) {
        const Uuid id = s.uuid(*it);
        if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) selection_.push_back(id);
    }
    // The clicked entity becomes the primary (inspected) selection.
    const Uuid tid = s.uuid(to);
    selection_.erase(std::find(selection_.begin(), selection_.end(), tid));
    selection_.push_back(tid);
}

std::size_t Editor::box_select(math::Vec2 a, math::Vec2 b, math::Vec2 size, bool additive) {
    Scene& s = active_scene();
    if (!additive) selection_.clear();
    if (size.x <= 0 || size.y <= 0) return 0;
    const math::Vec2 lo{std::min(a.x, b.x), std::min(a.y, b.y)}, hi{std::max(a.x, b.x), std::max(a.y, b.y)};
    const auto inside = [&](math::Vec2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; };
    std::vector<Entity> hits;
    const bool two_d = runtime::scene_view_mode(s) == runtime::ViewMode::TwoD;
    if (two_d) {
        const render::Camera2D cam = camera_.camera2d(size.y);
        s.world().each<WorldTransform>([&](Entity e, const WorldTransform& wt) {
            if (!s.world().has<SpriteComponent>(e) && !s.world().has<tilemap::TilemapComponent>(e)) return;
            const math::Vec3 p = wt.matrix.transform_point({0, 0, 0});
            if (inside(cam.world_to_screen({p.x, p.y}, size.x, size.y))) hits.push_back(e);
        });
    } else {
        const render::Camera3D cam = camera_.camera3d();
        const math::Mat4 vp = cam.projection(size.x / size.y) * cam.view;
        s.world().each<WorldTransform>([&](Entity e, const WorldTransform& wt) {
            bool has_mesh = false;
            const math::Aabb box = entity_bounds(e, &has_mesh);
            if (!has_mesh && !s.world().has<CameraComponent>(e) && !s.world().has<LightComponent>(e)) return;
            const math::Vec3 c = wt.matrix.transform_point((box.min + box.max) * 0.5f);
            const math::Vec4 clip = vp * math::Vec4{c.x, c.y, c.z, 1.0f};
            if (clip.w <= 1e-4f) return; // behind the camera
            const math::Vec2 px{(clip.x / clip.w * 0.5f + 0.5f) * size.x, (0.5f - clip.y / clip.w * 0.5f) * size.y};
            if (inside(px)) hits.push_back(e);
        });
    }
    // Parts of a prefab instance select the instance (like clicking them in the hierarchy would
    // select the part; a box is about objects).
    std::unordered_set<Uuid, UuidHash> seen(selection_.begin(), selection_.end());
    std::size_t added = 0;
    for (Entity e : hits) {
        if (const Entity root = instance_root(s, e); !root.is_null()) e = root;
        const Uuid id = s.uuid(e);
        if (!seen.insert(id).second) continue;
        selection_.push_back(id);
        ++added;
    }
    return added;
}

void Editor::apply_multi_edit(Entity primary, const json::Value& before) {
    Scene& s = active_scene();
    if (selection_.size() < 2) return;
    const json::Value after = s.save_entity(primary);
    const json::Value& bc = before["components"];
    const json::Value& ac = after["components"];
    if (bc == ac) return;
    const Uuid pid = s.uuid(primary);
    for (const Uuid& id : selection_) {
        if (id == pid) continue;
        const Entity e = s.find(id);
        if (e.is_null()) continue;
        json::Value snap = s.save_entity(e);
        json::Value merged = merge_edit(bc, ac, snap["components"]);
        if (merged == snap["components"]) continue;
        snap.set("components", std::move(merged));
        (void)s.restore_entity(snap);
    }
}

} // namespace victus::editor
