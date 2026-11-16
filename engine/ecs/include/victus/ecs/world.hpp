// SPDX-License-Identifier: Apache-2.0
// VICTUS ECS: archetype-based structure-of-arrays entity/component storage.
//
// - Entities are generational 32+32-bit handles; stale handles are detected.
// - Each unique component set is an Archetype holding one tightly packed column per component.
// - Queries match archetypes by signature (include/exclude) and are cached incrementally.
// - Iteration order is deterministic: archetypes in creation order, rows in insertion order
//   (destroy swaps the last row into the hole), which keeps lockstep simulation reproducible.
// - Structural changes while iterating are rejected (asserted); record them in a Commands
//   buffer and apply with World::flush().
#pragma once
#include "victus/core/assert.hpp"
#include "victus/core/jobs.hpp"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace victus::ecs {

struct Entity {
    std::uint32_t index{~0u};
    std::uint32_t generation{0};
    [[nodiscard]] constexpr bool is_null() const noexcept { return index == ~0u; }
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | index;
    }
    friend constexpr bool operator==(Entity, Entity) = default;
};
inline constexpr Entity kNullEntity{};

struct EntityHash {
    std::size_t operator()(Entity e) const noexcept { return std::hash<std::uint64_t>{}(e.bits()); }
};

using ComponentId = std::uint32_t;
inline constexpr std::uint32_t kMaxComponents = 512;
using Signature = std::bitset<kMaxComponents>;

// Type-erased description of a component type. Components may be C++ types (registered
// automatically on first use) or runtime-defined (scripting, editor data) with explicit ops.
struct ComponentInfo {
    std::string name;
    std::size_t size{0};
    std::size_t align{1};
    void (*construct)(void* dst){nullptr};
    void (*destroy)(void* dst){nullptr};
    void (*move_construct)(void* dst, void* src){nullptr}; // src remains valid; caller destroys it
    void (*copy_construct)(void* dst, const void* src){nullptr};
    void (*move_assign)(void* dst, void* src){nullptr};
};

// Process-wide component registry. Thread-safe registration; lookups are lock-free after
// the id is known.
class ComponentRegistry {
public:
    static ComponentRegistry& instance();
    ComponentId register_component(ComponentInfo info);
    [[nodiscard]] const ComponentInfo& info(ComponentId id) const;
    // Returns ~0u when unknown.
    [[nodiscard]] ComponentId find(std::string_view name) const;
    [[nodiscard]] std::size_t count() const;
    // Assigns a stable human readable name to a C++ component (used by serialization/editor).
    void set_name(ComponentId id, std::string name);

    template <typename T>
    ComponentId id_of();

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<ComponentInfo>> infos_;
    std::unordered_map<std::type_index, ComponentId> by_type_;
};

namespace detail {
template <typename T>
ComponentInfo make_info() {
    static_assert(std::is_move_constructible_v<T>, "components must be move constructible");
    static_assert(std::is_default_constructible_v<T>, "components must be default constructible");
    ComponentInfo info;
    info.name = typeid(T).name();
    info.size = sizeof(T);
    info.align = alignof(T);
    info.construct = [](void* dst) { new (dst) T(); };
    info.destroy = [](void* dst) { static_cast<T*>(dst)->~T(); };
    info.move_construct = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); };
    info.move_assign = [](void* dst, void* src) { *static_cast<T*>(dst) = std::move(*static_cast<T*>(src)); };
    if constexpr (std::is_copy_constructible_v<T>) {
        info.copy_construct = [](void* dst, const void* src) { new (dst) T(*static_cast<const T*>(src)); };
    }
    return info;
}
} // namespace detail

template <typename T>
ComponentId ComponentRegistry::id_of() {
    using U = std::remove_cvref_t<T>;
    static const ComponentId cached = [this] {
        {
            std::lock_guard lock(mutex_);
            const auto it = by_type_.find(std::type_index(typeid(U)));
            if (it != by_type_.end()) return it->second;
        }
        const ComponentId id = register_component(detail::make_info<U>());
        std::lock_guard lock(mutex_);
        by_type_.emplace(std::type_index(typeid(U)), id);
        return id;
    }();
    return cached;
}

template <typename T>
[[nodiscard]] inline ComponentId component_id() {
    return ComponentRegistry::instance().id_of<T>();
}

// One packed array per component inside an archetype.
class Column {
public:
    Column(ComponentId id, const ComponentInfo* info) : id_(id), info_(info) {}
    ~Column();
    Column(Column&& other) noexcept;
    Column& operator=(Column&&) = delete;
    Column(const Column&) = delete;

    [[nodiscard]] void* at(std::size_t row) noexcept { return data_ + row * info_->size; }
    [[nodiscard]] const void* at(std::size_t row) const noexcept { return data_ + row * info_->size; }
    [[nodiscard]] std::uint8_t* data() noexcept { return data_; }
    [[nodiscard]] ComponentId id() const noexcept { return id_; }
    [[nodiscard]] const ComponentInfo& info() const noexcept { return *info_; }
    void reserve(std::size_t rows, std::size_t live_rows);

private:
    ComponentId id_;
    const ComponentInfo* info_;
    std::uint8_t* data_{nullptr};
    std::size_t capacity_{0};
};

class Archetype {
public:
    Archetype(std::uint32_t index, Signature signature, std::vector<ComponentId> ids);
    ~Archetype();
    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;
    [[nodiscard]] const Signature& signature() const noexcept { return signature_; }
    [[nodiscard]] const std::vector<ComponentId>& ids() const noexcept { return ids_; }
    [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }
    [[nodiscard]] const std::vector<Entity>& entities() const noexcept { return entities_; }
    [[nodiscard]] std::uint32_t index() const noexcept { return index_; }
    // Column index for a component, or -1.
    [[nodiscard]] int column_index(ComponentId id) const noexcept {
        return id < column_of_.size() ? column_of_[id] : -1;
    }
    [[nodiscard]] Column& column(int i) noexcept { return columns_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] const Column& column(int i) const noexcept { return columns_[static_cast<std::size_t>(i)]; }
    template <typename T>
    [[nodiscard]] T* column_data() noexcept {
        const int c = column_index(component_id<T>());
        return c < 0 ? nullptr : reinterpret_cast<T*>(columns_[static_cast<std::size_t>(c)].data());
    }

private:
    friend class World;
    std::uint32_t push_uninitialized(Entity e);
    // Destroys the components at `row` and moves the last row into it. Returns the entity that
    // moved into `row` (or null when `row` was the last row).
    Entity swap_remove(std::uint32_t row);
    void clear();
    void reserve(std::size_t rows);

    std::uint32_t index_;
    Signature signature_;
    std::vector<ComponentId> ids_;
    std::vector<int> column_of_;
    std::vector<Column> columns_;
    std::vector<Entity> entities_;
    std::size_t capacity_{0};
    std::unordered_map<ComponentId, Archetype*> add_edges_, remove_edges_;
};

class World;

// Records structural changes for deferred application (e.g. from inside a query).
class Commands {
public:
    void destroy(Entity e);
    template <typename T>
    void add(Entity e, T value);
    template <typename T>
    void remove(Entity e);
    // Creates an entity at flush time and calls `init` with it.
    void spawn(std::function<void(World&, Entity)> init);
    // Arbitrary deferred structural work.
    void run(std::function<void(World&)> fn) { ops_.push_back(std::move(fn)); }
    void apply(World& world);
    [[nodiscard]] bool empty() const noexcept { return ops_.empty(); }

private:
    std::vector<std::function<void(World&)>> ops_;
};

struct QueryDesc {
    Signature include;
    Signature exclude;
    friend bool operator==(const QueryDesc&, const QueryDesc&) = default;
};

struct WorldStats {
    std::size_t alive{0};
    std::size_t archetypes{0};
    std::size_t cached_queries{0};
};

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] Entity create();
    void destroy(Entity e);
    [[nodiscard]] bool alive(Entity e) const noexcept;
    // Entity currently stored at `index`, regardless of generation (null if free).
    [[nodiscard]] Entity entity_at(std::uint32_t index) const noexcept;
    [[nodiscard]] std::size_t alive_count() const noexcept { return alive_; }
    void clear();

    template <typename T, typename... Args>
    T& add(Entity e, Args&&... args) {
        const ComponentId id = component_id<T>();
        if (void* existing = get_raw(e, id)) {
            T& ref = *static_cast<T*>(existing);
            ref = T{std::forward<Args>(args)...};
            return ref;
        }
        void* dst = add_raw_uninitialized(e, id);
        if constexpr (sizeof...(Args) == 0) {
            return *new (dst) T();
        } else {
            return *new (dst) T{std::forward<Args>(args)...};
        }
    }
    template <typename T>
    void remove(Entity e) {
        remove_raw(e, component_id<T>());
    }
    template <typename T>
    [[nodiscard]] T* get(Entity e) noexcept {
        return static_cast<T*>(get_raw(e, component_id<T>()));
    }
    template <typename T>
    [[nodiscard]] const T* get(Entity e) const noexcept {
        return static_cast<const T*>(get_raw(e, component_id<T>()));
    }
    template <typename T>
    [[nodiscard]] bool has(Entity e) const noexcept {
        return get_raw(e, component_id<T>()) != nullptr;
    }

    // Type-erased API (serialization, editor, scripting).
    void* add_raw(Entity e, ComponentId id);               // default constructs if missing
    void* add_raw_uninitialized(Entity e, ComponentId id); // caller must construct
    void remove_raw(Entity e, ComponentId id);
    [[nodiscard]] void* get_raw(Entity e, ComponentId id) noexcept;
    [[nodiscard]] const void* get_raw(Entity e, ComponentId id) const noexcept;
    [[nodiscard]] const std::vector<ComponentId>& components_of(Entity e) const;
    // Duplicates every copyable component of `src` onto a new entity.
    [[nodiscard]] Entity clone(Entity src);

    // Calls fn(Entity, Ts&...) or fn(Ts&...) for each entity having all Ts.
    template <typename... Ts, typename Fn>
    void each(Fn&& fn);
    // Same, excluding entities with any of the `Without` components.
    template <typename... Ts>
    struct Exclude {};
    template <typename... Ts, typename... Ws, typename Fn>
    void each_excluding(Exclude<Ws...>, Fn&& fn);
    // Parallel iteration over rows; fn must not touch structural state. Chunks of `grain` rows.
    template <typename... Ts, typename Fn>
    void par_each(jobs::JobSystem& jobs, std::size_t grain, Fn&& fn);
    template <typename... Ts>
    [[nodiscard]] std::size_t count();

    // Matching archetypes for a query, cached and updated incrementally.
    [[nodiscard]] const std::vector<Archetype*>& match(const QueryDesc& desc);
    [[nodiscard]] const std::vector<std::unique_ptr<Archetype>>& archetypes() const noexcept { return archetypes_; }

    void flush() { commands_.apply(*this); }
    [[nodiscard]] Commands& commands() noexcept { return commands_; }
    [[nodiscard]] bool iterating() const noexcept { return iterating_ > 0; }

    // World-global singletons ("resources"), owned by the world.
    template <typename T, typename... Args>
    T& set_resource(Args&&... args) {
        auto holder = std::make_shared<T>(std::forward<Args>(args)...);
        T& ref = *holder;
        resources_[std::type_index(typeid(T))] = std::move(holder);
        return ref;
    }
    template <typename T>
    [[nodiscard]] T* resource() noexcept {
        const auto it = resources_.find(std::type_index(typeid(T)));
        return it == resources_.end() ? nullptr : static_cast<T*>(it->second.get());
    }

    [[nodiscard]] WorldStats stats() const noexcept;

private:
    struct Record {
        Archetype* archetype{nullptr};
        std::uint32_t row{0};
        std::uint32_t generation{0};
    };
    struct DescHash {
        std::size_t operator()(const QueryDesc& d) const noexcept {
            return std::hash<Signature>{}(d.include) ^ (std::hash<Signature>{}(d.exclude) * 31u);
        }
    };
    struct QueryCache {
        std::vector<Archetype*> archetypes;
        std::size_t scanned{0};
    };
    Archetype* archetype_for(const Signature& sig);
    Archetype* with_component(Archetype* from, ComponentId id);
    Archetype* without_component(Archetype* from, ComponentId id);
    // Moves entity to `to`; returns the new row. Components in both are moved; components only
    // in `from` are destroyed; components only in `to` are left uninitialized.
    std::uint32_t move_entity(Entity e, Record& rec, Archetype* to);
    [[nodiscard]] const Record* record(Entity e) const noexcept;

    template <typename... Ts>
    static QueryDesc desc_of() {
        QueryDesc d;
        (d.include.set(component_id<Ts>()), ...);
        return d;
    }
    template <typename... Ts, typename Fn>
    void run_rows(Archetype& a, std::size_t begin, std::size_t end, Fn& fn);

    std::vector<Record> records_;
    std::vector<std::uint32_t> free_;
    std::size_t alive_{0};
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::unordered_map<Signature, Archetype*> by_signature_;
    std::unordered_map<QueryDesc, QueryCache, DescHash> queries_;
    Archetype* empty_{nullptr};
    Commands commands_;
    int iterating_{0};
    std::unordered_map<std::type_index, std::shared_ptr<void>> resources_;
};

// ---- template implementation -------------------------------------------------------------

template <typename T>
void Commands::add(Entity e, T value) {
    ops_.push_back([e, v = std::move(value)](World& w) mutable {
        if (w.alive(e)) w.add<T>(e, std::move(v));
    });
}
template <typename T>
void Commands::remove(Entity e) {
    ops_.push_back([e](World& w) {
        if (w.alive(e)) w.remove<T>(e);
    });
}

template <typename... Ts, typename Fn>
void World::run_rows(Archetype& a, std::size_t begin, std::size_t end, Fn& fn) {
    auto columns = std::make_tuple(a.column_data<Ts>()...);
    const Entity* entities = a.entities().data();
    for (std::size_t r = begin; r < end; ++r) {
        if constexpr (std::is_invocable_v<Fn&, Entity, Ts&...>) {
            std::apply([&](auto*... cols) { fn(entities[r], cols[r]...); }, columns);
        } else {
            std::apply([&](auto*... cols) { fn(cols[r]...); }, columns);
        }
    }
}

template <typename... Ts, typename Fn>
void World::each(Fn&& fn) {
    const auto& matched = match(desc_of<Ts...>());
    ++iterating_;
    for (Archetype* a : matched) run_rows<Ts...>(*a, 0, a->size(), fn);
    --iterating_;
}

template <typename... Ts, typename... Ws, typename Fn>
void World::each_excluding(Exclude<Ws...>, Fn&& fn) {
    QueryDesc d = desc_of<Ts...>();
    (d.exclude.set(component_id<Ws>()), ...);
    const auto& matched = match(d);
    ++iterating_;
    for (Archetype* a : matched) run_rows<Ts...>(*a, 0, a->size(), fn);
    --iterating_;
}

template <typename... Ts>
std::size_t World::count() {
    std::size_t n = 0;
    for (Archetype* a : match(desc_of<Ts...>())) n += a->size();
    return n;
}

template <typename... Ts, typename Fn>
void World::par_each(jobs::JobSystem& jobs, std::size_t grain, Fn&& fn) {
    const auto& matched = match(desc_of<Ts...>());
    ++iterating_;
    for (Archetype* a : matched) {
        if (a->size() == 0) continue;
        jobs.parallel_for(0, a->size(), std::max<std::size_t>(grain, 1),
                          [&](std::size_t b, std::size_t e) { run_rows<Ts...>(*a, b, e, fn); });
    }
    --iterating_;
}

} // namespace victus::ecs
