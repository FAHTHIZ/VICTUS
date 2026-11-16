// SPDX-License-Identifier: Apache-2.0
// Component reflection: one declaration per component drives JSON serialization, the editor
// inspector, "Add Component" menus and script access. Hand-written serializers remain supported
// for components with unusual layouts; reflected components are the default for new systems.
//
//     scene::Schema<RigidBodyComponent>("RigidBody", "Physics 3D")
//         .enumeration(&RigidBodyComponent::type, "type", {"static", "kinematic", "dynamic"})
//         .field(&RigidBodyComponent::mass, "mass", {.min = 0.001f, .max = 1e6f})
//         .commit();
#pragma once
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/ecs/world.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace victus::scene {

enum class FieldKind : std::uint8_t { Bool, Int, UInt, Float, Vec2, Vec3, Color, String, Enum, Uuid };

// Optional presentation/validation hints. min == max means unbounded.
struct FieldOptions {
    float min{0};
    float max{0};
    float speed{0};           // drag speed in the inspector (0 = automatic)
    const char* label{nullptr};   // inspector label (defaults to the serialized name)
    const char* tooltip{nullptr};
    const char* hint{nullptr};    // "bitmask16", "texture", "audio", "model", "degrees", "color_hdr"...
};

struct FieldInfo {
    std::string name;  // serialized key
    std::string label; // inspector label
    std::string tooltip;
    std::string hint;
    FieldKind kind{FieldKind::Float};
    std::size_t offset{0};
    std::size_t size{0}; // bytes of the underlying value (Int/UInt/Enum)
    float min{0}, max{0}, speed{0};
    std::vector<std::string> enum_names; // Enum: value i <-> enum_names[i]

    [[nodiscard]] bool bounded() const noexcept { return max > min; }
    template <typename T>
    [[nodiscard]] T& ref(void* component) const noexcept {
        return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(component) + offset);
    }
    template <typename T>
    [[nodiscard]] const T& ref(const void* component) const noexcept {
        return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(component) + offset);
    }
    // Integer view of Int/UInt/Enum fields regardless of their byte size.
    [[nodiscard]] std::int64_t get_int(const void* component) const noexcept;
    void set_int(void* component, std::int64_t v) const noexcept;
};

struct ComponentSchema {
    std::string name;     // serialized component name, e.g. "RigidBody"
    std::string category; // inspector/menu grouping, e.g. "Physics 3D"
    ecs::ComponentId id{0};
    std::vector<FieldInfo> fields;
    [[nodiscard]] const FieldInfo* field(std::string_view n) const noexcept {
        for (const auto& f : fields)
            if (f.name == n) return &f;
        return nullptr;
    }
    [[nodiscard]] json::Value save(const void* component) const;
    // Missing keys keep their current values; type mismatches and unknown enum names fail.
    [[nodiscard]] Status load(void* component, const json::Value& v) const;
};

// Registers a schema and a matching serializer. Returns the stored schema (stable address).
const ComponentSchema& register_schema(ComponentSchema schema);
[[nodiscard]] const ComponentSchema* find_schema(ecs::ComponentId id);
[[nodiscard]] const ComponentSchema* find_schema(std::string_view name);
// All reflected components in registration order.
[[nodiscard]] std::vector<const ComponentSchema*> schemas();

namespace detail {
template <typename T, typename M>
std::size_t member_offset(M T::*member) {
    static const T probe{};
    return static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(&(probe.*member)) -
                                    reinterpret_cast<const std::uint8_t*>(&probe));
}
template <typename M>
constexpr FieldKind kind_of() {
    if constexpr (std::is_same_v<M, bool>) return FieldKind::Bool;
    else if constexpr (std::is_same_v<M, float>) return FieldKind::Float;
    else if constexpr (std::is_same_v<M, math::Vec2>) return FieldKind::Vec2;
    else if constexpr (std::is_same_v<M, math::Vec3>) return FieldKind::Vec3;
    else if constexpr (std::is_same_v<M, math::Color>) return FieldKind::Color;
    else if constexpr (std::is_same_v<M, std::string>) return FieldKind::String;
    else if constexpr (std::is_same_v<M, Uuid>) return FieldKind::Uuid;
    else if constexpr (std::is_integral_v<M> && std::is_signed_v<M>) return FieldKind::Int;
    else if constexpr (std::is_integral_v<M> && std::is_unsigned_v<M>) return FieldKind::UInt;
    else static_assert(sizeof(M) == 0, "unsupported reflected field type");
}
} // namespace detail

template <typename T>
class Schema {
public:
    Schema(std::string name, std::string category) {
        schema_.name = std::move(name);
        schema_.category = std::move(category);
        schema_.id = ecs::component_id<T>();
    }
    template <typename M>
    Schema& field(M T::*member, std::string name, FieldOptions o = {}) {
        static_assert(!std::is_enum_v<M>, "use enumeration() for enum fields");
        FieldInfo f = make(std::move(name), o);
        f.kind = detail::kind_of<M>();
        f.offset = detail::member_offset(member);
        f.size = sizeof(M);
        schema_.fields.push_back(std::move(f));
        return *this;
    }
    template <typename E>
    Schema& enumeration(E T::*member, std::string name, std::initializer_list<const char*> names, FieldOptions o = {}) {
        static_assert(std::is_enum_v<E>, "enumeration() requires an enum member");
        FieldInfo f = make(std::move(name), o);
        f.kind = FieldKind::Enum;
        f.offset = detail::member_offset(member);
        f.size = sizeof(E);
        for (const char* n : names) f.enum_names.emplace_back(n);
        schema_.fields.push_back(std::move(f));
        return *this;
    }
    const ComponentSchema& commit() { return register_schema(std::move(schema_)); }

private:
    static FieldInfo make(std::string name, const FieldOptions& o) {
        FieldInfo f;
        f.label = o.label ? o.label : name;
        f.name = std::move(name);
        if (o.tooltip) f.tooltip = o.tooltip;
        if (o.hint) f.hint = o.hint;
        f.min = o.min;
        f.max = o.max;
        f.speed = o.speed;
        return f;
    }
    ComponentSchema schema_;
};

} // namespace victus::scene
