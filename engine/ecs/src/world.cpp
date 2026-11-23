// SPDX-License-Identifier: Apache-2.0
#include "victus/ecs/world.hpp"

#include <cstring>

namespace victus::ecs {

// ---- registry ------------------------------------------------------------------------------

ComponentRegistry& ComponentRegistry::instance() {
    static ComponentRegistry registry;
    return registry;
}

ComponentId ComponentRegistry::register_component(ComponentInfo info) {
    VICTUS_VERIFY(info.size > 0 && info.construct && info.destroy && info.move_construct,
                  "component info requires size and lifetime operations");
    std::lock_guard lock(mutex_);
    const auto id = static_cast<ComponentId>(infos_.size());
    VICTUS_VERIFY(id < kMaxComponents, "too many component types (raise ecs::kMaxComponents)");
    infos_.push_back(std::make_unique<ComponentInfo>(std::move(info)));
    return id;
}

const ComponentInfo& ComponentRegistry::info(ComponentId id) const {
    std::lock_guard lock(mutex_);
    VICTUS_VERIFY(id < infos_.size(), "unknown component id");
    return *infos_[id];
}

ComponentId ComponentRegistry::find(std::string_view name) const {
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < infos_.size(); ++i) {
        if (infos_[i]->name == name) return static_cast<ComponentId>(i);
    }
    return ~0u;
}

std::size_t ComponentRegistry::count() const {
    std::lock_guard lock(mutex_);
    return infos_.size();
}

void ComponentRegistry::set_name(ComponentId id, std::string name) {
    std::lock_guard lock(mutex_);
    VICTUS_VERIFY(id < infos_.size(), "unknown component id");
    infos_[id]->name = std::move(name);
}

// ---- column ----------------------------------------------------------------------------------

Column::~Column() {
    if (data_) ::operator delete(data_, std::align_val_t(info_->align));
}

Column::Column(Column&& other) noexcept
    : id_(other.id_), info_(other.info_), data_(other.data_), capacity_(other.capacity_) {
    other.data_ = nullptr;
    other.capacity_ = 0;
}

void Column::reserve(std::size_t rows, std::size_t live_rows) {
    if (rows <= capacity_) return;
    auto* fresh = static_cast<std::uint8_t*>(::operator new(rows * info_->size, std::align_val_t(info_->align)));
    for (std::size_t r = 0; r < live_rows; ++r) {
        void* src = data_ + r * info_->size;
        info_->move_construct(fresh + r * info_->size, src);
        info_->destroy(src);
    }
    if (data_) ::operator delete(data_, std::align_val_t(info_->align));
    data_ = fresh;
    capacity_ = rows;
}

// ---- archetype -------------------------------------------------------------------------------

Archetype::Archetype(std::uint32_t index, Signature signature, std::vector<ComponentId> ids)
    : index_(index), signature_(signature), ids_(std::move(ids)) {
    const ComponentId max_id = ids_.empty() ? 0 : ids_.back();
    column_of_.assign(ids_.empty() ? 0 : max_id + 1, -1);
    columns_.reserve(ids_.size());
    auto& registry = ComponentRegistry::instance();
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        column_of_[ids_[i]] = static_cast<int>(i);
        columns_.emplace_back(ids_[i], &registry.info(ids_[i]));
    }
}

Archetype::~Archetype() { clear(); }

void Archetype::clear() {
    for (auto& column : columns_) {
        for (std::size_t r = 0; r < entities_.size(); ++r) column.info().destroy(column.at(r));
    }
    entities_.clear();
}

void Archetype::reserve(std::size_t rows) {
    if (rows <= capacity_) return;
    for (auto& column : columns_) column.reserve(rows, entities_.size());
    entities_.reserve(rows);
    capacity_ = rows;
}

std::uint32_t Archetype::push_uninitialized(Entity e) {
    if (entities_.size() == capacity_) reserve(std::max<std::size_t>(16, capacity_ * 2));
    entities_.push_back(e);
    return static_cast<std::uint32_t>(entities_.size() - 1);
}

Entity Archetype::swap_remove(std::uint32_t row) {
    const auto last = static_cast<std::uint32_t>(entities_.size() - 1);
    for (auto& column : columns_) {
        const ComponentInfo& info = column.info();
        info.destroy(column.at(row));
        if (row != last) {
            info.move_construct(column.at(row), column.at(last));
            info.destroy(column.at(last));
        }
    }
    Entity moved = kNullEntity;
    if (row != last) {
        moved = entities_[last];
        entities_[row] = moved;
    }
    entities_.pop_back();
    return moved;
}

// ---- commands --------------------------------------------------------------------------------

void Commands::destroy(Entity e) {
    ops_.push_back([e](World& w) {
        if (w.alive(e)) w.destroy(e);
    });
}

void Commands::spawn(std::function<void(World&, Entity)> init) {
    ops_.push_back([init = std::move(init)](World& w) {
        const Entity e = w.create();
        init(w, e);
    });
}

void Commands::apply(World& world) {
    // Ops may enqueue further ops; drain until stable.
    while (!ops_.empty()) {
        std::vector<std::function<void(World&)>> batch;
        batch.swap(ops_);
        for (auto& op : batch) op(world);
    }
}

// ---- world -----------------------------------------------------------------------------------

World::World() { empty_ = archetype_for(Signature{}); }

World::~World() {
    archetypes_.clear(); // destroys components before resources
    resources_.clear();
}

Archetype* World::archetype_for(const Signature& sig) {
    if (const auto it = by_signature_.find(sig); it != by_signature_.end()) return it->second;
    std::vector<ComponentId> ids;
    for (std::size_t i = 0; i < kMaxComponents; ++i) {
        if (sig.test(i)) ids.push_back(static_cast<ComponentId>(i));
    }
    auto arch = std::make_unique<Archetype>(static_cast<std::uint32_t>(archetypes_.size()), sig, std::move(ids));
    Archetype* raw = arch.get();
    archetypes_.push_back(std::move(arch));
    by_signature_.emplace(sig, raw);
    return raw;
}

Archetype* World::with_component(Archetype* from, ComponentId id) {
    if (const auto it = from->add_edges_.find(id); it != from->add_edges_.end()) return it->second;
    Signature sig = from->signature();
    sig.set(id);
    Archetype* to = archetype_for(sig);
    from->add_edges_[id] = to;
    to->remove_edges_[id] = from;
    return to;
}

Archetype* World::without_component(Archetype* from, ComponentId id) {
    if (const auto it = from->remove_edges_.find(id); it != from->remove_edges_.end()) return it->second;
    Signature sig = from->signature();
    sig.reset(id);
    Archetype* to = archetype_for(sig);
    from->remove_edges_[id] = to;
    to->add_edges_[id] = from;
    return to;
}

const World::Record* World::record(Entity e) const noexcept {
    if (e.index >= records_.size()) return nullptr;
    const Record& rec = records_[e.index];
    if (rec.generation != e.generation || rec.archetype == nullptr) return nullptr;
    return &rec;
}

Entity World::create() {
    VICTUS_ASSERT(!iterating(), "structural change during iteration; use World::commands()");
    std::uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(records_.size());
        records_.push_back(Record{nullptr, 0, 1});
    }
    Record& rec = records_[index];
    const Entity e{index, rec.generation};
    rec.archetype = empty_;
    rec.row = empty_->push_uninitialized(e);
    ++alive_;
    return e;
}

void World::destroy(Entity e) {
    VICTUS_ASSERT(!iterating(), "structural change during iteration; use World::commands()");
    if (!alive(e)) return;
    ++structural_version_;
    Record& rec = records_[e.index];
    const Entity moved = rec.archetype->swap_remove(rec.row);
    if (!moved.is_null()) records_[moved.index].row = rec.row;
    rec.archetype = nullptr;
    rec.row = 0;
    ++rec.generation;
    if (rec.generation == 0) rec.generation = 1;
    free_.push_back(e.index);
    --alive_;
}

bool World::alive(Entity e) const noexcept { return record(e) != nullptr; }

Entity World::entity_at(std::uint32_t index) const noexcept {
    if (index >= records_.size() || records_[index].archetype == nullptr) return kNullEntity;
    return Entity{index, records_[index].generation};
}

void World::clear() {
    VICTUS_ASSERT(!iterating(), "structural change during iteration");
    ++structural_version_;
    for (auto& arch : archetypes_) arch->clear();
    free_.clear();
    for (std::uint32_t i = static_cast<std::uint32_t>(records_.size()); i-- > 0;) {
        Record& rec = records_[i];
        if (rec.archetype) {
            ++rec.generation;
            if (rec.generation == 0) rec.generation = 1;
            rec.archetype = nullptr;
        }
        free_.push_back(i);
    }
    alive_ = 0;
}

std::uint32_t World::move_entity(Entity e, Record& rec, Archetype* to) {
    ++structural_version_;
    Archetype* from = rec.archetype;
    const std::uint32_t new_row = to->push_uninitialized(e);
    for (std::size_t i = 0; i < to->ids().size(); ++i) {
        const int src = from->column_index(to->ids()[i]);
        if (src < 0) continue;
        Column& dst_col = to->column(static_cast<int>(i));
        dst_col.info().move_construct(dst_col.at(new_row), from->column(src).at(rec.row));
    }
    const Entity moved = from->swap_remove(rec.row);
    if (!moved.is_null()) records_[moved.index].row = rec.row;
    rec.archetype = to;
    rec.row = new_row;
    return new_row;
}

void* World::add_raw_uninitialized(Entity e, ComponentId id) {
    VICTUS_ASSERT(!iterating(), "structural change during iteration; use World::commands()");
    VICTUS_VERIFY(alive(e), "add on a dead entity");
    Record& rec = records_[e.index];
    VICTUS_ASSERT(rec.archetype->column_index(id) < 0, "component already present");
    Archetype* to = with_component(rec.archetype, id);
    const std::uint32_t row = move_entity(e, rec, to);
    return to->column(to->column_index(id)).at(row);
}

void* World::add_raw(Entity e, ComponentId id) {
    if (void* existing = get_raw(e, id)) return existing;
    void* dst = add_raw_uninitialized(e, id);
    ComponentRegistry::instance().info(id).construct(dst);
    return dst;
}

void World::remove_raw(Entity e, ComponentId id) {
    VICTUS_ASSERT(!iterating(), "structural change during iteration; use World::commands()");
    if (!alive(e)) return;
    Record& rec = records_[e.index];
    if (rec.archetype->column_index(id) < 0) return;
    move_entity(e, rec, without_component(rec.archetype, id));
}

void* World::get_raw(Entity e, ComponentId id) noexcept {
    const Record* rec = record(e);
    if (!rec) return nullptr;
    const int c = rec->archetype->column_index(id);
    return c < 0 ? nullptr : rec->archetype->column(c).at(rec->row);
}

const void* World::get_raw(Entity e, ComponentId id) const noexcept {
    const Record* rec = record(e);
    if (!rec) return nullptr;
    const int c = rec->archetype->column_index(id);
    return c < 0 ? nullptr : rec->archetype->column(c).at(rec->row);
}

const std::vector<ComponentId>& World::components_of(Entity e) const {
    static const std::vector<ComponentId> none;
    const Record* rec = record(e);
    return rec ? rec->archetype->ids() : none;
}

Entity World::clone(Entity src) {
    VICTUS_VERIFY(alive(src), "clone of a dead entity");
    Signature sig;
    for (const ComponentId id : records_[src.index].archetype->ids()) {
        if (ComponentRegistry::instance().info(id).copy_construct) sig.set(id);
    }
    const Entity dst = create();
    Record& dst_rec = records_[dst.index];
    Archetype* to = archetype_for(sig);
    if (to != empty_) move_entity(dst, dst_rec, to);
    const Record& src_rec = records_[src.index]; // re-read: rows may have moved
    for (std::size_t i = 0; i < to->ids().size(); ++i) {
        Column& col = to->column(static_cast<int>(i));
        const Archetype* sa = src_rec.archetype;
        const void* from = sa->column(sa->column_index(col.id())).at(src_rec.row);
        col.info().copy_construct(col.at(dst_rec.row), from);
    }
    return dst;
}

const std::vector<Archetype*>& World::match(const QueryDesc& desc) {
    QueryCache& cache = queries_[desc];
    for (; cache.scanned < archetypes_.size(); ++cache.scanned) {
        Archetype* a = archetypes_[cache.scanned].get();
        if ((a->signature() & desc.include) == desc.include && (a->signature() & desc.exclude).none()) {
            cache.archetypes.push_back(a);
        }
    }
    return cache.archetypes;
}

WorldStats World::stats() const noexcept {
    return WorldStats{alive_, archetypes_.size(), queries_.size()};
}

} // namespace victus::ecs
