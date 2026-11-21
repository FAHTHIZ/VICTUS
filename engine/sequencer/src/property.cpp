// SPDX-License-Identifier: Apache-2.0
// Property tracks (ADR-0190): reading and writing component fields as up to four floats. Built-in
// components (Camera, Light, MeshRenderer, Sprite) are listed by hand; every reflected component
// (scene::Schema) is covered through its field table.
#include "victus/scene/reflect.hpp"
#include "victus/sequencer/sequencer.hpp"

#include <algorithm>
#include <cmath>

namespace victus::sequencer {

namespace {

enum class Slot : std::uint8_t { Float, Vec2, Vec3, Color, Int, Bool };

struct Bound {
    void* data{nullptr};
    Slot slot{Slot::Float};
    const scene::FieldInfo* field{nullptr}; // reflected: range, integer size
    void* component{nullptr};               // reflected: the component the field lives in
};

struct Builtin {
    const char* component;
    const char* property;
    const char* label;
    Slot slot;
    void* (*get)(scene::Scene&, Entity);
};

template <class C, auto Member> void* member_of(scene::Scene& s, Entity e) {
    C* c = s.world().get<C>(e);
    return c ? static_cast<void*>(&(c->*Member)) : nullptr;
}

const Builtin kBuiltins[] = {
    {"Camera", "fov_y", "Field of view (degrees)", Slot::Float,
     member_of<scene::CameraComponent, &scene::CameraComponent::fov_y_degrees>},
    {"Camera", "ortho_height", "Orthographic height", Slot::Float,
     member_of<scene::CameraComponent, &scene::CameraComponent::ortho_height>},
    {"Light", "color", "Colour", Slot::Color, member_of<scene::LightComponent, &scene::LightComponent::color>},
    {"Light", "intensity", "Intensity", Slot::Float, member_of<scene::LightComponent, &scene::LightComponent::intensity>},
    {"Light", "range", "Range", Slot::Float, member_of<scene::LightComponent, &scene::LightComponent::range>},
    {"Light", "inner_angle", "Inner angle (degrees)", Slot::Float,
     member_of<scene::LightComponent, &scene::LightComponent::inner_angle_degrees>},
    {"Light", "outer_angle", "Outer angle (degrees)", Slot::Float,
     member_of<scene::LightComponent, &scene::LightComponent::outer_angle_degrees>},
    {"MeshRenderer", "base_color", "Base colour", Slot::Color,
     member_of<scene::MeshRendererComponent, &scene::MeshRendererComponent::base_color>},
    {"MeshRenderer", "metallic", "Metallic", Slot::Float,
     member_of<scene::MeshRendererComponent, &scene::MeshRendererComponent::metallic>},
    {"MeshRenderer", "roughness", "Roughness", Slot::Float,
     member_of<scene::MeshRendererComponent, &scene::MeshRendererComponent::roughness>},
    {"Sprite", "tint", "Tint", Slot::Color, member_of<scene::SpriteComponent, &scene::SpriteComponent::tint>},
    {"Sprite", "size", "Size", Slot::Vec2, member_of<scene::SpriteComponent, &scene::SpriteComponent::size>},
};

std::uint8_t channels_of(Slot s) noexcept {
    switch (s) {
    case Slot::Vec2: return 2;
    case Slot::Vec3: return 3;
    case Slot::Color: return 4;
    default: return 1;
    }
}

std::optional<Slot> slot_of(scene::FieldKind k) noexcept {
    switch (k) {
    case scene::FieldKind::Float: return Slot::Float;
    case scene::FieldKind::Vec2: return Slot::Vec2;
    case scene::FieldKind::Vec3: return Slot::Vec3;
    case scene::FieldKind::Color: return Slot::Color;
    case scene::FieldKind::Int:
    case scene::FieldKind::UInt: return Slot::Int;
    case scene::FieldKind::Bool: return Slot::Bool;
    default: return std::nullopt;
    }
}

std::optional<Bound> bind(scene::Scene& s, Entity e, std::string_view component, std::string_view property) {
    if (e.is_null() || !s.world().alive(e)) return std::nullopt;
    for (const Builtin& b : kBuiltins)
        if (component == b.component && property == b.property) {
            void* p = b.get(s, e);
            if (!p) return std::nullopt;
            return Bound{p, b.slot, nullptr, nullptr};
        }
    const scene::ComponentSchema* schema = scene::find_schema(component);
    if (!schema) return std::nullopt;
    const scene::FieldInfo* f = schema->field(property);
    if (!f) return std::nullopt;
    const auto slot = slot_of(f->kind);
    if (!slot) return std::nullopt;
    void* c = s.world().get_raw(e, schema->id);
    if (!c) return std::nullopt;
    return Bound{static_cast<std::uint8_t*>(c) + f->offset, *slot, f, c};
}

} // namespace

std::vector<AnimatableProperty> animatable_properties(scene::Scene& s, Entity e) {
    std::vector<AnimatableProperty> out;
    if (e.is_null() || !s.world().alive(e)) return out;
    for (const Builtin& b : kBuiltins)
        if (b.get(s, e))
            out.push_back({b.component, b.property, b.label, channels_of(b.slot), b.slot == Slot::Color, false});
    for (const scene::ComponentSchema* schema : scene::schemas()) {
        if (!s.world().get_raw(e, schema->id)) continue;
        for (const scene::FieldInfo& f : schema->fields) {
            const auto slot = slot_of(f.kind);
            if (!slot) continue;
            out.push_back({schema->name, f.name, f.label.empty() ? f.name : f.label, channels_of(*slot),
                           *slot == Slot::Color, *slot == Slot::Int || *slot == Slot::Bool});
        }
    }
    return out;
}

std::optional<math::Vec4> read_property(scene::Scene& s, Entity e, std::string_view component, std::string_view property) {
    const auto b = bind(s, e, component, property);
    if (!b) return std::nullopt;
    switch (b->slot) {
    case Slot::Float: return math::Vec4{*static_cast<const float*>(b->data), 0, 0, 0};
    case Slot::Vec2: {
        const auto& v = *static_cast<const math::Vec2*>(b->data);
        return math::Vec4{v.x, v.y, 0, 0};
    }
    case Slot::Vec3: return math::Vec4{*static_cast<const math::Vec3*>(b->data), 0};
    case Slot::Color: {
        const auto& c = *static_cast<const math::Color*>(b->data);
        return math::Vec4{c.r, c.g, c.b, c.a};
    }
    case Slot::Int: return math::Vec4{static_cast<float>(b->field->get_int(b->component)), 0, 0, 0};
    case Slot::Bool: return math::Vec4{*static_cast<const bool*>(b->data) ? 1.0f : 0.0f, 0, 0, 0};
    }
    return std::nullopt;
}

bool write_property(scene::Scene& s, Entity e, std::string_view component, std::string_view property, math::Vec4 v) {
    const auto b = bind(s, e, component, property);
    if (!b) return false;
    const auto clamp = [&](float x) {
        if (!std::isfinite(x)) return 0.0f;
        return b->field && b->field->bounded() ? std::clamp(x, b->field->min, b->field->max) : x;
    };
    switch (b->slot) {
    case Slot::Float: *static_cast<float*>(b->data) = clamp(v.x); break;
    case Slot::Vec2: *static_cast<math::Vec2*>(b->data) = {clamp(v.x), clamp(v.y)}; break;
    case Slot::Vec3: *static_cast<math::Vec3*>(b->data) = {clamp(v.x), clamp(v.y), clamp(v.z)}; break;
    case Slot::Color: *static_cast<math::Color*>(b->data) = {clamp(v.x), clamp(v.y), clamp(v.z), clamp(v.w)}; break;
    case Slot::Int: b->field->set_int(b->component, std::llround(std::clamp(clamp(v.x), -1e15f, 1e15f))); break;
    case Slot::Bool: *static_cast<bool*>(b->data) = v.x > 0.5f; break;
    }
    return true;
}

} // namespace victus::sequencer
