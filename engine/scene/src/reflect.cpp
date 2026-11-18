// SPDX-License-Identifier: Apache-2.0
#include "victus/scene/reflect.hpp"

#include "victus/scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace victus::scene {

using json::Value;

std::int64_t FieldInfo::get_int(const void* c) const noexcept {
    const auto* p = static_cast<const std::uint8_t*>(c) + offset;
    const bool is_signed = kind == FieldKind::Int;
    switch (size) {
    case 1: {
        std::uint8_t v;
        std::memcpy(&v, p, 1);
        return is_signed ? static_cast<std::int8_t>(v) : v;
    }
    case 2: {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return is_signed ? static_cast<std::int16_t>(v) : v;
    }
    case 4: {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return is_signed ? static_cast<std::int32_t>(v) : static_cast<std::int64_t>(v);
    }
    default: {
        std::int64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    }
}

void FieldInfo::set_int(void* c, std::int64_t v) const noexcept {
    auto* p = static_cast<std::uint8_t*>(c) + offset;
    switch (size) {
    case 1: {
        const auto x = static_cast<std::uint8_t>(v);
        std::memcpy(p, &x, 1);
        break;
    }
    case 2: {
        const auto x = static_cast<std::uint16_t>(v);
        std::memcpy(p, &x, 2);
        break;
    }
    case 4: {
        const auto x = static_cast<std::uint32_t>(v);
        std::memcpy(p, &x, 4);
        break;
    }
    default: std::memcpy(p, &v, 8); break;
    }
}

namespace {
Status field_error(const ComponentSchema& s, const FieldInfo& f, std::string_view why) {
    return make_error(ErrorCode::ParseError, s.name + "." + f.name + ": " + std::string(why));
}

struct SchemaStore {
    std::mutex mutex;
    std::vector<std::unique_ptr<ComponentSchema>> list;
    static SchemaStore& get() {
        static SchemaStore s;
        return s;
    }
};
} // namespace

Value ComponentSchema::save(const void* c) const {
    Value v = Value::object();
    for (const auto& f : fields) {
        switch (f.kind) {
        case FieldKind::Bool: v.set(f.name, f.ref<bool>(c)); break;
        case FieldKind::Int:
        case FieldKind::UInt: v.set(f.name, Value(f.get_int(c))); break;
        case FieldKind::Float: v.set(f.name, f.ref<float>(c)); break;
        case FieldKind::Vec2: v.set(f.name, to_json(f.ref<math::Vec2>(c))); break;
        case FieldKind::Vec3: v.set(f.name, to_json(f.ref<math::Vec3>(c))); break;
        case FieldKind::Color: v.set(f.name, to_json(f.ref<math::Color>(c))); break;
        case FieldKind::String: v.set(f.name, f.ref<std::string>(c)); break;
        case FieldKind::Uuid: {
            const Uuid& u = f.ref<Uuid>(c);
            v.set(f.name, u.is_nil() ? Value() : Value(u.to_string()));
            break;
        }
        case FieldKind::Vec2List: {
            Value list = Value::array();
            for (const math::Vec2& p : f.ref<std::vector<math::Vec2>>(c)) list.push(to_json(p));
            v.set(f.name, std::move(list));
            break;
        }
        case FieldKind::Enum: {
            const auto i = static_cast<std::size_t>(f.get_int(c));
            v.set(f.name, i < f.enum_names.size() ? Value(f.enum_names[i]) : Value(static_cast<std::int64_t>(i)));
            break;
        }
        }
    }
    return v;
}

Status ComponentSchema::load(void* c, const Value& v) const {
    if (!v.is_object()) return make_error(ErrorCode::ParseError, name + ": expected an object");
    for (const auto& f : fields) {
        const Value* x = v.find(f.name);
        if (!x || x->is_null()) {
            if (x && f.kind == FieldKind::Uuid) f.ref<Uuid>(c) = {};
            continue;
        }
        switch (f.kind) {
        case FieldKind::Bool:
            if (!x->is_bool()) return field_error(*this, f, "expected a boolean");
            f.ref<bool>(c) = x->as_bool();
            break;
        case FieldKind::Int:
        case FieldKind::UInt: {
            if (!x->is_number() || !std::isfinite(x->as_number())) return field_error(*this, f, "expected an integer");
            std::int64_t i = x->as_int();
            if (f.kind == FieldKind::UInt && i < 0) return field_error(*this, f, "expected a non-negative integer");
            if (f.bounded()) i = std::clamp<std::int64_t>(i, static_cast<std::int64_t>(f.min), static_cast<std::int64_t>(f.max));
            f.set_int(c, i);
            break;
        }
        case FieldKind::Float: {
            if (!x->is_number() || !std::isfinite(x->as_number())) return field_error(*this, f, "expected a finite number");
            float fv = x->as_float();
            if (f.bounded()) fv = std::clamp(fv, f.min, f.max);
            f.ref<float>(c) = fv;
            break;
        }
        case FieldKind::Vec2:
            if (!from_json(*x, f.ref<math::Vec2>(c))) return field_error(*this, f, "expected [x, y]");
            break;
        case FieldKind::Vec3:
            if (!from_json(*x, f.ref<math::Vec3>(c))) return field_error(*this, f, "expected [x, y, z]");
            break;
        case FieldKind::Color:
            if (!from_json(*x, f.ref<math::Color>(c))) return field_error(*this, f, "expected [r, g, b(, a)]");
            break;
        case FieldKind::String:
            if (!x->is_string()) return field_error(*this, f, "expected a string");
            f.ref<std::string>(c) = x->as_string();
            break;
        case FieldKind::Uuid: {
            Uuid u;
            if (!x->is_string() || !Uuid::parse(x->as_string(), u)) return field_error(*this, f, "expected a UUID string");
            f.ref<Uuid>(c) = u;
            break;
        }
        case FieldKind::Vec2List: {
            if (!x->is_array()) return field_error(*this, f, "expected [[x, y], ...]");
            if (x->size() > kMaxListLength)
                return field_error(*this, f, "more than " + std::to_string(kMaxListLength) + " entries");
            std::vector<math::Vec2> list(x->size());
            for (std::size_t i = 0; i < list.size(); ++i)
                if (!from_json((*x)[i], list[i])) return field_error(*this, f, "expected [[x, y], ...]");
            f.ref<std::vector<math::Vec2>>(c) = std::move(list);
            break;
        }
        case FieldKind::Enum: {
            std::int64_t idx = -1;
            if (x->is_string()) {
                for (std::size_t i = 0; i < f.enum_names.size(); ++i)
                    if (f.enum_names[i] == x->as_string()) idx = static_cast<std::int64_t>(i);
            } else if (x->is_number()) {
                idx = x->as_int();
            }
            if (idx < 0 || idx >= static_cast<std::int64_t>(f.enum_names.size()))
                return field_error(*this, f, "unknown value");
            f.set_int(c, idx);
            break;
        }
        }
    }
    return {};
}

const ComponentSchema& register_schema(ComponentSchema schema) {
    auto& store = SchemaStore::get();
    const ComponentSchema* stored = nullptr;
    {
        std::lock_guard lock(store.mutex);
        const auto existing = std::find_if(store.list.begin(), store.list.end(),
                                           [&](const auto& e) { return e->name == schema.name; });
        if (existing != store.list.end()) {
            **existing = std::move(schema); // re-registration replaces the schema in place
            stored = existing->get();
        } else {
            store.list.push_back(std::make_unique<ComponentSchema>(std::move(schema)));
            stored = store.list.back().get();
        }
    }
    ComponentSerializer s;
    s.name = stored->name;
    s.id = stored->id;
    ecs::ComponentRegistry::instance().set_name(s.id, s.name);
    s.save = [stored](const void* p) { return stored->save(p); };
    s.load = [stored](void* p, const Value& v) { return stored->load(p, v); };
    SerializerRegistry::instance().add(std::move(s));
    return *stored;
}

const ComponentSchema* find_schema(ecs::ComponentId id) {
    auto& store = SchemaStore::get();
    std::lock_guard lock(store.mutex);
    for (const auto& s : store.list)
        if (s->id == id) return s.get();
    return nullptr;
}

const ComponentSchema* find_schema(std::string_view name) {
    auto& store = SchemaStore::get();
    std::lock_guard lock(store.mutex);
    for (const auto& s : store.list)
        if (s->name == name) return s.get();
    return nullptr;
}

std::vector<const ComponentSchema*> schemas() {
    auto& store = SchemaStore::get();
    std::lock_guard lock(store.mutex);
    std::vector<const ComponentSchema*> out;
    out.reserve(store.list.size());
    for (const auto& s : store.list) out.push_back(s.get());
    return out;
}

} // namespace victus::scene
