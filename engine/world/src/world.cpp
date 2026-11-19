// SPDX-License-Identifier: Apache-2.0
#include "victus/world/world.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace victus::world {

using json::Value;

namespace {
Error bad(std::string m) { return make_error(ErrorCode::ParseError, "world: " + std::move(m)); }

Value aabb_to_json(const math::Aabb& b) {
    Value v = Value::object();
    v.set("min", scene::to_json(b.min));
    v.set("max", scene::to_json(b.max));
    return v;
}

bool read_uuid(const Value& v, Uuid& out) { return v.is_string() && Uuid::parse(v.as_string(), out); }

float distance_to(const math::Aabb& b, math::Vec3 p) noexcept {
    const math::Vec3 q = math::max(b.min, math::min(p, b.max));
    return math::length(p - q);
}
} // namespace

// --- Descriptor -------------------------------------------------------------------------------

const migration::Format& world_format() {
    static const migration::Format format("victus.world", kWorldFormatVersion);
    return format;
}

Result<WorldDesc> parse_world(const Value& source) {
    if (source["format"].as_string_or("") != "victus.world") return bad("not a victus.world document");
    Value doc = source;
    if (auto report = world_format().upgrade(doc); !report) return std::move(report).error();
    WorldDesc w;
    w.name = doc["name"].as_string_or("World");
    w.load_radius = doc["load_radius"].as_float(w.load_radius);
    w.unload_margin = doc["unload_margin"].as_float(w.unload_margin);
    if (!(w.load_radius > 0.0f) || !std::isfinite(w.load_radius)) return bad("load_radius must be positive");
    if (!(w.unload_margin >= 0.0f) || !std::isfinite(w.unload_margin)) return bad("unload_margin must be zero or positive");
    const Value& cells = doc["cells"];
    if (!cells.is_array()) return bad("cells must be an array");
    std::unordered_set<Uuid, UuidHash> ids;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const Value& c = cells[i];
        CellDesc d;
        const std::string where = "cell " + std::to_string(i);
        if (!c.is_object() || !read_uuid(c["id"], d.id) || d.id.is_nil()) return bad(where + ": missing or invalid id");
        if (!ids.insert(d.id).second) return bad(where + ": duplicate id " + d.id.to_string());
        d.name = c["name"].as_string_or(d.id.to_string());
        d.scene = c["scene"].as_string_or("");
        if (d.scene.empty()) return bad(where + ": missing scene path");
        if (!fs::sanitize_relative(d.scene)) return bad(where + ": scene path must be relative to the project");
        const Value& b = c["bounds"];
        if (!b.is_object() || !scene::from_json(b["min"], d.bounds.min) || !scene::from_json(b["max"], d.bounds.max) ||
            !d.bounds.valid())
            return bad(where + ": invalid bounds");
        d.load_radius = c["load_radius"].as_float(0.0f);
        if (!(d.load_radius >= 0.0f) || !std::isfinite(d.load_radius)) return bad(where + ": load_radius must be zero or positive");
        w.cells.push_back(std::move(d));
    }
    return w;
}

Value world_to_json(const WorldDesc& w) {
    Value v = Value::object();
    v.set("format", "victus.world");
    v.set("version", kWorldFormatVersion);
    v.set("name", w.name);
    v.set("load_radius", w.load_radius);
    v.set("unload_margin", w.unload_margin);
    Value cells = Value::array();
    for (const CellDesc& c : w.cells) {
        Value cv = Value::object();
        cv.set("id", c.id.to_string());
        cv.set("name", c.name);
        cv.set("scene", c.scene);
        cv.set("bounds", aabb_to_json(c.bounds));
        if (c.load_radius > 0.0f) cv.set("load_radius", c.load_radius);
        cells.push(std::move(cv));
    }
    v.set("cells", std::move(cells));
    return v;
}

Result<WorldDesc> load_world_file(const fs::Path& path) {
    auto text = fs::read_text(path);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return std::move(doc).error();
    return parse_world(*doc);
}

Status save_world_file(const WorldDesc& world, const fs::Path& path) {
    return fs::write_text_atomic(path, json::stringify(world_to_json(world), true));
}

// --- Components -------------------------------------------------------------------------------

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        using S = StreamingSourceComponent;
        scene::Schema<S>("StreamingSource", "World")
            .field(&S::radius_scale, "radius_scale", {.min = 0, .max = 100, .speed = 0.01f, .tooltip = "Multiplies every cell's load radius around this entity"})
            .field(&S::enabled, "enabled", {.tooltip = "Load world cells around this entity"})
            .commit();
        using W = WorldStreamingComponent;
        scene::Schema<W>("WorldStreaming", "World")
            .field(&W::world, "world", {.tooltip = "Project-relative .vworld file whose cells stream around the streaming sources", .hint = "file"})
            .commit();
    });
}

std::vector<StreamingSource> gather_sources(scene::Scene& scene, const math::Vec3* fallback) {
    std::vector<StreamingSource> out;
    scene.world().each<StreamingSourceComponent, scene::WorldTransform>(
        [&](const StreamingSourceComponent& s, const scene::WorldTransform& wt) {
            if (s.enabled) out.push_back({wt.matrix.transform_point({}), std::max(s.radius_scale, 0.0f)});
        });
    if (out.empty() && fallback) out.push_back({*fallback, 1.0f});
    return out;
}

std::string_view cell_state_name(CellState s) noexcept {
    switch (s) {
    case CellState::Unloaded: return "unloaded";
    case CellState::Loading: return "loading";
    case CellState::Waiting: return "waiting";
    case CellState::Active: return "active";
    case CellState::Failed: return "failed";
    }
    return "unknown";
}

// --- Cell documents ---------------------------------------------------------------------------

Result<Value> read_cell(const fs::Path& path) {
    auto text = fs::read_text(path);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return std::move(doc).error();
    if ((*doc)["format"].as_string_or("") != "victus.scene") return bad(path.string() + " is not a victus.scene document");
    if (!(*doc)["entities"].is_array()) return bad(path.string() + ": entities must be an array");
    return doc;
}

namespace {
void collect_uuids(const Value& v, std::vector<Uuid>& out) {
    if (v.is_string()) {
        const std::string& s = v.as_string();
        Uuid id;
        if (s.size() == 36 && Uuid::parse(s, id) && !id.is_nil()) out.push_back(id);
    } else if (v.is_array()) {
        for (const Value& e : v.as_array()) collect_uuids(e, out);
    } else if (v.is_object()) {
        for (const auto& [k, e] : v.as_object()) collect_uuids(e, out);
    }
}
} // namespace

std::vector<Uuid> referenced_uuids(const Value& doc) {
    std::unordered_set<Uuid, UuidHash> own;
    const Value& entities = doc["entities"];
    for (std::size_t i = 0; i < entities.size(); ++i) {
        Uuid id;
        if (read_uuid(entities[i]["uuid"], id)) own.insert(id);
    }
    std::vector<Uuid> all;
    for (std::size_t i = 0; i < entities.size(); ++i) collect_uuids(entities[i]["components"], all);
    std::vector<Uuid> out;
    std::unordered_set<Uuid, UuidHash> seen;
    for (const Uuid& id : all)
        if (!own.contains(id) && seen.insert(id).second) out.push_back(id);
    return out;
}

// --- Streamer ---------------------------------------------------------------------------------

struct WorldStreamer::Job {
    fs::Path path;
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};
    std::optional<Result<Value>> result; // written by the worker before `done`
};

WorldStreamer::WorldStreamer(WorldDesc world, fs::Path root, jobs::JobSystem* jobs, Dependencies deps, StreamerSettings settings)
    : world_(std::move(world)), root_(std::move(root)), jobs_(jobs), deps_(std::move(deps)), settings_(settings) {
    for (const CellDesc& d : world_.cells) cells_[d.id].desc = &d;
}

WorldStreamer::~WorldStreamer() { wait_idle(); }

void WorldStreamer::wait_idle() {
    if (jobs_) jobs_->wait(counter_);
}

void WorldStreamer::start_load(Cell& cell) {
    cell.state = CellState::Loading;
    cell.waited = 0;
    if (cell.saved) { // state captured at the last unload replaces the file
        cell.doc = *cell.saved;
        cell.deps = referenced_uuids(cell.doc);
        if (deps_.request)
            for (const Uuid& d : cell.deps) deps_.request(d);
        cell.state = CellState::Waiting;
        return;
    }
    auto path = fs::resolve_under(root_, cell.desc->scene);
    if (!path) {
        cell.state = CellState::Failed;
        cell.error = std::move(path).error();
        return;
    }
    auto job = std::make_shared<Job>();
    job->path = *path;
    cell.job = job;
    if (!jobs_) {
        job->result.emplace(read_cell(job->path));
        job->done.store(true, std::memory_order_release);
        return;
    }
    jobs_->submit(
        [job] {
            VICTUS_PROFILE_SCOPE("WorldStreamer::read_cell");
            if (!job->cancelled.load(std::memory_order_acquire)) job->result.emplace(read_cell(job->path));
            job->done.store(true, std::memory_order_release);
        },
        &counter_, jobs::Priority::Background);
}

void WorldStreamer::collect(Cell& cell) {
    if (cell.state != CellState::Loading || !cell.job || !cell.job->done.load(std::memory_order_acquire)) return;
    auto job = std::move(cell.job);
    if (!job->result) { // cancelled before it ran
        cell.state = CellState::Unloaded;
        return;
    }
    if (!*job->result) {
        cell.state = CellState::Failed;
        cell.error = std::move(*job->result).error();
        VICTUS_LOG_ERROR("world", "cell {} ({}): {}", cell.desc->name, cell.desc->scene, cell.error.message);
        return;
    }
    cell.doc = std::move(*job->result).value();
    cell.deps = referenced_uuids(cell.doc);
    if (deps_.request)
        for (const Uuid& d : cell.deps) deps_.request(d);
    cell.state = CellState::Waiting;
}

void WorldStreamer::activate(scene::Scene& scene, Cell& cell) {
    VICTUS_PROFILE_SCOPE("WorldStreamer::activate");
    std::vector<scene::Entity> created;
    if (auto st = scene.load_additive(cell.doc, &created); !st.ok()) {
        cell.state = CellState::Failed;
        cell.error = st.error();
        cell.doc = Value();
        VICTUS_LOG_ERROR("world", "cell {}: {}", cell.desc->name, cell.error.message);
        return;
    }
    cell.roots.clear();
    for (const scene::Entity e : created) {
        scene.world().add<CellMember>(e, CellMember{cell.desc->id});
        // Roots of the cell: entities whose parent is not part of it (a scene root or a
        // persistent entity the cell attached to).
        const scene::Entity p = scene.parent(e);
        const auto* pm = p.is_null() ? nullptr : scene.world().get<CellMember>(p);
        if (!pm || pm->cell != cell.desc->id) cell.roots.push_back(scene.uuid(e));
    }
    cell.entities = static_cast<std::uint32_t>(created.size());
    cell.doc = Value();
    cell.deps.clear();
    cell.state = CellState::Active;
    ++totals_.loads;
}

Value WorldStreamer::snapshot(const scene::Scene& scene, const Cell& cell) const {
    Value doc = Value::object();
    doc.set("format", "victus.scene");
    doc.set("version", scene::kSceneFormatVersion);
    doc.set("name", cell.desc->name);
    Value entities = Value::array();
    for (const Uuid& id : cell.roots) {
        const scene::Entity e = scene.find(id);
        if (e.is_null()) continue; // destroyed by gameplay: stays destroyed
        const Value sub = scene.save_subtree(e);
        for (std::size_t i = 0; i < sub.size(); ++i) {
            Value ev = sub[i];
            // The snapshot's root keeps its parent only when that parent is persistent.
            if (i == 0 && scene.parent(e).is_null()) ev.set("parent", Value());
            if (i == 0) ev.set("next", Value());
            entities.push(std::move(ev));
        }
    }
    doc.set("entities", std::move(entities));
    return doc;
}

void WorldStreamer::deactivate(scene::Scene& scene, Cell& cell) {
    VICTUS_PROFILE_SCOPE("WorldStreamer::deactivate");
    cell.saved = snapshot(scene, cell);
    for (const Uuid& id : cell.roots)
        if (const scene::Entity e = scene.find(id); !e.is_null()) scene.destroy(e);
    cell.roots.clear();
    cell.entities = 0;
    cell.state = CellState::Unloaded;
    ++totals_.unloads;
}

void WorldStreamer::update(scene::Scene& scene, std::span<const StreamingSource> sources) {
    VICTUS_PROFILE_SCOPE("WorldStreamer::update");
    ++totals_.updates;
    struct Want {
        Cell* cell;
        float distance;
    };
    std::vector<Want> to_load;
    for (auto& [id, cell] : cells_) {
        collect(cell);
        const float radius = cell.desc->load_radius > 0.0f ? cell.desc->load_radius : world_.load_radius;
        float nearest = std::numeric_limits<float>::infinity(); // in units of each source's radius
        bool keep = false;
        for (const StreamingSource& s : sources) {
            const float r = radius * s.radius_scale;
            const float d = distance_to(cell.desc->bounds, s.position);
            if (d <= r) nearest = std::min(nearest, r > 0.0f ? d / r : 0.0f);
            if (d <= r + world_.unload_margin * s.radius_scale) keep = true;
        }
        const bool want = std::isfinite(nearest);
        switch (cell.state) {
        case CellState::Unloaded:
            if (want) to_load.push_back({&cell, nearest});
            break;
        case CellState::Loading:
            if (!keep) { // cancel: a running read finishes and is discarded
                if (cell.job) cell.job->cancelled.store(true, std::memory_order_release);
                cell.job.reset();
                cell.state = CellState::Unloaded;
                ++totals_.cancelled;
            }
            break;
        case CellState::Waiting:
            if (!keep) {
                cell.doc = Value();
                cell.deps.clear();
                cell.state = CellState::Unloaded;
                ++totals_.cancelled;
            }
            break;
        case CellState::Active:
            if (!keep) deactivate(scene, cell);
            break;
        case CellState::Failed: break; // stays failed (logged once)
        }
    }
    // Start loads nearest first, within the concurrency limit.
    std::uint32_t loading = 0;
    for (const auto& [id, cell] : cells_)
        if (cell.state == CellState::Loading) ++loading;
    std::sort(to_load.begin(), to_load.end(), [](const Want& a, const Want& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.cell->desc->name < b.cell->desc->name; // deterministic order
    });
    for (const Want& w : to_load) {
        if (loading >= std::max(settings_.max_loading, 1u)) break;
        start_load(*w.cell);
        if (w.cell->state == CellState::Loading) ++loading;
        collect(*w.cell); // synchronous reads are done already
    }
    // Activate cells whose dependencies are resident, nearest first (the to_load order is stale
    // for cells that waited, so sort the waiting ones by distance again).
    std::vector<Want> ready;
    for (auto& [id, cell] : cells_) {
        if (cell.state != CellState::Waiting) continue;
        bool all = true;
        if (deps_.ready)
            for (const Uuid& d : cell.deps)
                if (!deps_.ready(d)) {
                    all = false;
                    break;
                }
        if (!all && ++cell.waited <= settings_.max_wait_updates) continue;
        if (!all) {
            ++totals_.dependency_timeouts;
            VICTUS_LOG_WARN("world", "cell {}: assets still loading after {} updates; activating anyway", cell.desc->name,
                            settings_.max_wait_updates);
        }
        float nearest = std::numeric_limits<float>::infinity();
        for (const StreamingSource& s : sources) nearest = std::min(nearest, distance_to(cell.desc->bounds, s.position));
        ready.push_back({&cell, nearest});
    }
    std::sort(ready.begin(), ready.end(), [](const Want& a, const Want& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.cell->desc->name < b.cell->desc->name;
    });
    std::uint32_t activated = 0;
    for (const Want& w : ready) {
        if (activated >= std::max(settings_.activations_per_update, 1u)) break;
        activate(scene, *w.cell);
        ++activated;
    }
}

void WorldStreamer::unload_all(scene::Scene& scene) {
    for (auto& [id, cell] : cells_) {
        switch (cell.state) {
        case CellState::Active: deactivate(scene, cell); break;
        case CellState::Loading:
            if (cell.job) cell.job->cancelled.store(true, std::memory_order_release);
            cell.job.reset();
            cell.state = CellState::Unloaded;
            break;
        case CellState::Waiting:
            cell.doc = Value();
            cell.state = CellState::Unloaded;
            break;
        default: break;
        }
    }
}

CellState WorldStreamer::state(const Uuid& cell) const {
    const auto it = cells_.find(cell);
    return it == cells_.end() ? CellState::Unloaded : it->second.state;
}

const Error* WorldStreamer::error(const Uuid& cell) const {
    const auto it = cells_.find(cell);
    return it != cells_.end() && it->second.state == CellState::Failed ? &it->second.error : nullptr;
}

StreamerStats WorldStreamer::stats() const {
    StreamerStats s = totals_;
    for (const auto& [id, c] : cells_) {
        switch (c.state) {
        case CellState::Active:
            ++s.active;
            s.active_entities += c.entities;
            break;
        case CellState::Loading: ++s.loading; break;
        case CellState::Waiting: ++s.waiting; break;
        case CellState::Failed: ++s.failed; break;
        default: break;
        }
    }
    return s;
}

Value WorldStreamer::save_state(const scene::Scene& scene) const {
    // Deterministic order: the world's cell order.
    Value cells = Value::object();
    for (const CellDesc& d : world_.cells) {
        const Cell& c = cells_.at(d.id);
        if (c.state == CellState::Active) cells.set(d.id.to_string(), snapshot(scene, c));
        else if (c.saved) cells.set(d.id.to_string(), *c.saved);
    }
    Value v = Value::object();
    v.set("format", "victus.world_state");
    v.set("version", 1);
    v.set("cells", std::move(cells));
    return v;
}

Status WorldStreamer::load_state(const Value& state) {
    if (state["format"].as_string_or("") != "victus.world_state") return bad("not a victus.world_state document");
    const Value& cells = state["cells"];
    if (!cells.is_object()) return bad("world state: cells must be an object");
    for (const auto& [key, doc] : cells.as_object()) {
        Uuid id;
        if (!Uuid::parse(key, id)) return bad("world state: invalid cell id " + key);
        const auto it = cells_.find(id);
        if (it == cells_.end()) {
            VICTUS_LOG_WARN("world", "saved state for unknown cell {} ignored", key);
            continue;
        }
        if (it->second.state == CellState::Active || it->second.state == CellState::Waiting)
            return make_error(ErrorCode::AlreadyExists, "world state: cell " + it->second.desc->name + " is already loaded");
        if (doc["format"].as_string_or("") != "victus.scene" || !doc["entities"].is_array())
            return bad("world state: cell " + key + " is not a victus.scene document");
        it->second.saved = doc;
    }
    return {};
}

// --- Partition --------------------------------------------------------------------------------

namespace {
bool holds_persistent(const Value& subtree) {
    static const char* kPersistent[] = {"Camera", "Environment", "AudioListener", "Script", "StreamingSource",
                                        "WorldStreaming", "UiCanvas", "Terrain", "Tilemap", "NetworkIdentity",
                                        "SequencePlayer", "CharacterController", "CharacterController2D",
                                        "IrradianceVolume", "NavMeshSurface"};
    for (std::size_t i = 0; i < subtree.size(); ++i) {
        const Value& comps = subtree[i]["components"];
        if (!comps.is_object()) continue;
        for (const char* name : kPersistent)
            if (comps.find(name)) return true;
        if (const Value* light = comps.find("Light"); light && (*light)["type"].as_string_or("") == "directional") return true;
    }
    return false;
}
} // namespace

Result<Partition> partition_scene(const scene::Scene& scene, const PartitionSettings& settings, const std::string& world_path) {
    if (!(settings.cell_size > 0.0f) || !std::isfinite(settings.cell_size)) return bad("cell_size must be positive");
    if (!fs::sanitize_relative(settings.directory)) return bad("cell directory must be relative to the project");
    Partition out;
    out.world.name = settings.name;
    out.world.load_radius = settings.load_radius > 0.0f ? settings.load_radius : settings.cell_size * 1.5f;
    out.world.unload_margin = settings.cell_size * 0.25f;
    struct Bucket {
        Value entities = Value::array();
        float min_y{std::numeric_limits<float>::max()}, max_y{-std::numeric_limits<float>::max()};
    };
    std::map<std::pair<long, long>, Bucket> buckets; // ordered: stable cell order
    Value persistent = scene.save();
    Value kept = Value::array();
    std::unordered_set<Uuid, UuidHash> moved;
    for (const scene::Entity root : scene.roots()) {
        const Value sub = scene.save_subtree(root);
        if (holds_persistent(sub)) continue;
        const math::Mat4 m = scene.world_matrix(root);
        // Content wider than a cell (ground planes, large colliders) would pop in and out as a
        // unit and could drop characters through the floor: it stays persistent.
        const float sx = math::length(math::Vec3{m.c[0].x, m.c[0].y, m.c[0].z});
        const float sz = math::length(math::Vec3{m.c[2].x, m.c[2].y, m.c[2].z});
        if (std::max(sx, sz) > settings.cell_size) continue;
        const math::Vec3 p = m.transform_point({});
        const std::pair<long, long> key{static_cast<long>(std::floor(p.x / settings.cell_size)),
                                        static_cast<long>(std::floor(p.z / settings.cell_size))};
        Bucket& b = buckets[key];
        for (std::size_t i = 0; i < sub.size(); ++i) {
            Value ev = sub[i];
            if (i == 0) ev.set("next", Value());
            Uuid id;
            if (read_uuid(ev["uuid"], id)) moved.insert(id);
            b.entities.push(std::move(ev));
        }
        b.min_y = std::min(b.min_y, p.y);
        b.max_y = std::max(b.max_y, p.y);
    }
    const Value& all = persistent["entities"];
    for (std::size_t i = 0; i < all.size(); ++i) {
        Uuid id;
        if (read_uuid(all[i]["uuid"], id) && moved.contains(id)) continue;
        kept.push(all[i]);
    }
    // The persistent scene names the world it streams.
    Value marker = Value::object();
    marker.set("uuid", Uuid::generate().to_string());
    marker.set("name", "World Streaming");
    Value comps = Value::object();
    Value ws = Value::object();
    ws.set("world", world_path);
    comps.set("WorldStreaming", std::move(ws));
    marker.set("components", std::move(comps));
    kept.push(std::move(marker));
    persistent.set("entities", std::move(kept));
    out.persistent = std::move(persistent);
    out.moved_entities = moved.size();

    const std::string dir = *fs::sanitize_relative(settings.directory);
    for (auto& [key, b] : buckets) {
        CellDesc d;
        d.id = Uuid::generate();
        d.name = "cell_" + std::to_string(key.first) + "_" + std::to_string(key.second);
        d.scene = (dir.empty() ? std::string() : dir + "/") + d.name + ".vscene";
        const float half = settings.cell_size * 0.5f;
        d.bounds.min = {static_cast<float>(key.first) * settings.cell_size, b.min_y - half, static_cast<float>(key.second) * settings.cell_size};
        d.bounds.max = {d.bounds.min.x + settings.cell_size, b.max_y + half, d.bounds.min.z + settings.cell_size};
        Value doc = Value::object();
        doc.set("format", "victus.scene");
        doc.set("version", scene::kSceneFormatVersion);
        doc.set("name", d.name);
        doc.set("entities", std::move(b.entities));
        out.cells.emplace_back(d.scene, std::move(doc));
        out.world.cells.push_back(std::move(d));
    }
    return out;
}

} // namespace victus::world
