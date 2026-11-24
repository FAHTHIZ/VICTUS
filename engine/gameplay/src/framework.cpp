// SPDX-License-Identifier: Apache-2.0
// Gameplay framework primitives (ADR-0094).
#include "victus/gameplay/framework.hpp"

#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <mutex>

namespace victus::gameplay {

using json::Value;

namespace {

bool tag_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += ' ';
        out += p;
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

} // namespace

// ---- tags ----------------------------------------------------------------------------------------

bool valid_tag(std::string_view tag) noexcept {
    if (tag.empty() || tag.size() > 128 || tag.front() == '.' || tag.back() == '.') return false;
    char prev = 0;
    for (const char c : tag) {
        if (!tag_char(c) || (c == '.' && prev == '.')) return false;
        prev = c;
    }
    return true;
}

bool tag_matches(std::string_view tag, std::string_view query) noexcept {
    if (query.empty() || tag.size() < query.size() || tag.compare(0, query.size(), query) != 0) return false;
    return tag.size() == query.size() || tag[query.size()] == '.';
}

std::vector<std::string> split_tags(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == ';' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
        std::size_t j = i;
        while (j < text.size() && !(text[j] == ' ' || text[j] == ',' || text[j] == ';' || text[j] == '\t' || text[j] == '\n' || text[j] == '\r')) ++j;
        if (j > i) {
            std::string t(text.substr(i, j - i));
            if (valid_tag(t) && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(std::move(t));
        }
        i = j;
    }
    return out;
}

std::vector<std::string> tags_of(scene::Scene& scene, Entity e) {
    std::vector<std::string> out;
    if (!scene.world().alive(e)) return out;
    if (const auto* t = scene.world().get<TagsComponent>(e)) out = split_tags(t->tags);
    if (const auto* a = scene.world().get<AttributesComponent>(e))
        for (const ActiveEffect& fx : a->effects)
            if (!fx.spec.grant_tag.empty() && std::find(out.begin(), out.end(), fx.spec.grant_tag) == out.end())
                out.push_back(fx.spec.grant_tag);
    return out;
}

bool has_tag(scene::Scene& scene, Entity e, std::string_view query) {
    if (!scene.world().alive(e)) return false;
    if (const auto* t = scene.world().get<TagsComponent>(e)) {
        // Scan in place: no allocation for the common query.
        std::string_view text = t->tags;
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && !tag_char(text[i])) ++i;
            std::size_t j = i;
            while (j < text.size() && tag_char(text[j])) ++j;
            if (j > i && tag_matches(text.substr(i, j - i), query)) return true;
            i = j;
        }
    }
    if (const auto* a = scene.world().get<AttributesComponent>(e))
        for (const ActiveEffect& fx : a->effects)
            if (tag_matches(fx.spec.grant_tag, query)) return true;
    return false;
}

bool add_tag(scene::Scene& scene, Entity e, std::string_view tag) {
    if (!scene.world().alive(e) || !valid_tag(tag)) return false;
    auto* t = scene.world().get<TagsComponent>(e);
    if (!t) t = &scene.world().add<TagsComponent>(e);
    std::vector<std::string> tags = split_tags(t->tags);
    if (std::find(tags.begin(), tags.end(), tag) != tags.end()) return false;
    tags.emplace_back(tag);
    t->tags = join(tags);
    return true;
}

bool remove_tag(scene::Scene& scene, Entity e, std::string_view tag) {
    auto* t = scene.world().alive(e) ? scene.world().get<TagsComponent>(e) : nullptr;
    if (!t) return false;
    std::vector<std::string> tags = split_tags(t->tags);
    const auto it = std::find(tags.begin(), tags.end(), tag);
    if (it == tags.end()) return false;
    tags.erase(it);
    t->tags = join(tags);
    return true;
}

std::vector<Entity> find_tagged(scene::Scene& scene, std::string_view query) {
    std::vector<Entity> candidates;
    scene.world().each<TagsComponent>([&](Entity e, TagsComponent&) { candidates.push_back(e); });
    scene.world().each<AttributesComponent>([&](Entity e, AttributesComponent& a) {
        if (!a.effects.empty() && !scene.world().get<TagsComponent>(e)) candidates.push_back(e);
    });
    std::vector<Entity> out;
    for (Entity e : candidates)
        if (has_tag(scene, e, query)) out.push_back(e);
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return out;
}

// ---- events --------------------------------------------------------------------------------------

namespace {

struct EventBus {
    std::vector<GameplayEvent> events;
    std::vector<std::uint64_t> ticks; // tick each event was emitted in
    std::uint64_t next_id{1};
    std::uint64_t tick{0};
    struct Listener {
        std::uint64_t handle;
        std::string name;
        EventListener fn;
    };
    std::vector<Listener> listeners;
    std::uint64_t next_listener{1};
    int depth{0};
    bool warned{false};
};

EventBus& bus(scene::Scene& scene) {
    if (auto* b = scene.world().resource<EventBus>()) return *b;
    return scene.world().set_resource<EventBus>();
}

} // namespace

std::uint64_t emit_event(scene::Scene& scene, std::string_view name, json::Value payload, Entity sender, Entity target) {
    EventBus& b = bus(scene);
    if (b.events.size() >= kMaxQueuedEvents) {
        if (!b.warned) VICTUS_LOG_WARN("gameplay", "event queue full ({} events); dropping the oldest", kMaxQueuedEvents);
        b.warned = true;
        b.events.erase(b.events.begin());
        b.ticks.erase(b.ticks.begin());
    }
    GameplayEvent ev{b.next_id++, std::string(name), std::move(payload), sender, target};
    b.events.push_back(ev);
    b.ticks.push_back(b.tick);
    if (b.depth < 16 && !b.listeners.empty()) {
        ++b.depth;
        // Copy: listeners may (un)register listeners.
        const std::vector<EventBus::Listener> listeners = b.listeners;
        for (const auto& l : listeners)
            if (l.fn && (l.name.empty() || l.name == ev.name)) l.fn(scene, ev);
        --bus(scene).depth;
    }
    return ev.id;
}

std::uint64_t listen(scene::Scene& scene, std::string name, EventListener fn) {
    EventBus& b = bus(scene);
    const std::uint64_t h = b.next_listener++;
    b.listeners.push_back({h, std::move(name), std::move(fn)});
    return h;
}

void unlisten(scene::Scene& scene, std::uint64_t handle) {
    auto& l = bus(scene).listeners;
    std::erase_if(l, [&](const EventBus::Listener& x) { return x.handle == handle; });
}

const std::vector<GameplayEvent>& recent_events(scene::Scene& scene) { return bus(scene).events; }

void begin_event_tick(scene::Scene& scene) {
    EventBus& b = bus(scene);
    ++b.tick;
    std::size_t keep = 0;
    while (keep < b.ticks.size() && b.ticks[keep] + 2 <= b.tick) ++keep;
    if (keep > 0) {
        b.events.erase(b.events.begin(), b.events.begin() + static_cast<std::ptrdiff_t>(keep));
        b.ticks.erase(b.ticks.begin(), b.ticks.begin() + static_cast<std::ptrdiff_t>(keep));
    }
}

// ---- timers --------------------------------------------------------------------------------------

void start_timer(scene::Scene& scene, Entity e, std::string_view name, float seconds, bool repeat) {
    if (!scene.world().alive(e) || name.empty()) return;
    auto* c = scene.world().get<TimersComponent>(e);
    if (!c) c = &scene.world().add<TimersComponent>(e);
    Timer t{std::string(name), std::max(0.0f, seconds), std::max(0.0f, seconds), repeat, false};
    for (Timer& x : c->timers)
        if (x.name == name) {
            x = std::move(t);
            return;
        }
    c->timers.push_back(std::move(t));
}

bool stop_timer(scene::Scene& scene, Entity e, std::string_view name) {
    auto* c = scene.world().alive(e) ? scene.world().get<TimersComponent>(e) : nullptr;
    return c && std::erase_if(c->timers, [&](const Timer& t) { return t.name == name; }) > 0;
}

bool pause_timer(scene::Scene& scene, Entity e, std::string_view name, bool paused) {
    auto* c = scene.world().alive(e) ? scene.world().get<TimersComponent>(e) : nullptr;
    if (!c) return false;
    for (Timer& t : c->timers)
        if (t.name == name) {
            t.paused = paused;
            return true;
        }
    return false;
}

std::optional<float> timer_remaining(scene::Scene& scene, Entity e, std::string_view name) {
    const auto* c = scene.world().alive(e) ? scene.world().get<TimersComponent>(e) : nullptr;
    if (!c) return std::nullopt;
    for (const Timer& t : c->timers)
        if (t.name == name) return t.remaining;
    return std::nullopt;
}

void update_timers(scene::Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("gameplay.timers");
    std::vector<std::pair<Entity, std::string>> fired;
    scene.world().each<TimersComponent>([&](Entity e, TimersComponent& c) {
        for (std::size_t i = 0; i < c.timers.size();) {
            Timer& t = c.timers[i];
            if (t.paused) {
                ++i;
                continue;
            }
            t.remaining -= dt;
            bool done = false;
            // A short repeating timer fires at most 8 times per tick (no runaway catch-up).
            for (int n = 0; t.remaining <= 0.0f && n < 8; ++n) {
                fired.emplace_back(e, t.name);
                if (!t.repeat || t.interval <= 0.0f) {
                    done = true;
                    break;
                }
                t.remaining += t.interval;
            }
            if (t.repeat && t.remaining <= 0.0f) t.remaining = t.interval;
            if (done) c.timers.erase(c.timers.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
    });
    for (auto& [e, name] : fired) {
        Value p = Value::object();
        p.set("timer", true);
        emit_event(scene, name, std::move(p), e, e);
    }
}

// ---- attributes ------------------------------------------------------------------------------------

namespace {

bool parse_float(std::string_view s, float& out) {
    s = trim(s);
    if (s.empty() || s.size() > 63) return false;
    char buf[64];
    std::copy(s.begin(), s.end(), buf);
    buf[s.size()] = 0;
    char* end = nullptr;
    const float v = std::strtof(buf, &end);
    if (end != buf + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool attribute_name(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return !(s.front() >= '0' && s.front() <= '9');
}

} // namespace

Result<std::vector<Attribute>> parse_attributes(std::string_view definition) {
    std::vector<Attribute> out;
    std::size_t start = 0;
    while (start <= definition.size()) {
        std::size_t end = definition.find_first_of(";\n", start);
        if (end == std::string_view::npos) end = definition.size();
        std::string_view entry = trim(definition.substr(start, end - start));
        start = end + 1;
        if (entry.empty()) continue;
        const auto fail = [&](std::string_view why) {
            return Error{ErrorCode::ParseError, std::format("attribute '{}': {}", entry, why)};
        };
        const std::size_t eq = entry.find('=');
        if (eq == std::string_view::npos) return fail("expected 'name = value'");
        Attribute a;
        a.name = std::string(trim(entry.substr(0, eq)));
        if (!attribute_name(a.name)) return fail("names use letters, digits and '_'");
        std::string_view rest = trim(entry.substr(eq + 1));
        std::string_view bounds;
        if (const std::size_t lb = rest.find('['); lb != std::string_view::npos) {
            const std::size_t rb = rest.find(']', lb);
            if (rb == std::string_view::npos) return fail("missing ']'");
            bounds = rest.substr(lb + 1, rb - lb - 1);
            rest = trim(rest.substr(0, lb));
        }
        if (!parse_float(rest, a.base)) return fail("value is not a number");
        if (!bounds.empty()) {
            const std::size_t dots = bounds.find("..");
            if (dots == std::string_view::npos) return fail("bounds are written [min..max]");
            const std::string_view lo = trim(bounds.substr(0, dots)), hi = trim(bounds.substr(dots + 2));
            if (!lo.empty() && !parse_float(lo, a.min)) {
                if (!attribute_name(lo)) return fail("min is not a number or attribute");
                a.min_attribute = std::string(lo);
            }
            if (!hi.empty() && !parse_float(hi, a.max)) {
                if (!attribute_name(hi)) return fail("max is not a number or attribute");
                a.max_attribute = std::string(hi);
            }
        }
        for (const Attribute& o : out)
            if (o.name == a.name) return fail("defined twice");
        a.value = a.base;
        out.push_back(std::move(a));
    }
    return out;
}

namespace {

Attribute* find_attr(AttributesComponent& c, std::string_view name) {
    for (Attribute& a : c.attributes)
        if (a.name == name) return &a;
    return nullptr;
}

float bound_value(const AttributesComponent& c, const Attribute& a, bool upper, int depth);

float evaluate(const AttributesComponent& c, const Attribute& a, int depth) {
    float add = 0.0f, mul = 1.0f;
    std::optional<float> over;
    for (const ActiveEffect& fx : c.effects) {
        if (fx.spec.attribute != a.name || fx.spec.duration == 0.0f || fx.spec.period > 0.0f) continue;
        const auto stacks = static_cast<float>(fx.stacks);
        switch (fx.spec.op) {
        case ModifierOp::Add: add += fx.spec.magnitude * stacks; break;
        case ModifierOp::Multiply: mul *= std::pow(fx.spec.magnitude, stacks); break;
        case ModifierOp::Override: over = fx.spec.magnitude; break;
        }
    }
    const float v = over ? *over : (a.base + add) * mul;
    return std::clamp(v, std::min(bound_value(c, a, false, depth), bound_value(c, a, true, depth)), bound_value(c, a, true, depth));
}

float bound_value(const AttributesComponent& c, const Attribute& a, bool upper, int depth) {
    const std::string& ref = upper ? a.max_attribute : a.min_attribute;
    const float fixed = upper ? a.max : a.min;
    if (ref.empty() || depth > 4) return fixed;
    for (const Attribute& o : c.attributes)
        if (o.name == ref && &o != &a) return upper ? std::min(fixed, evaluate(c, o, depth + 1)) : std::max(fixed, evaluate(c, o, depth + 1));
    return fixed;
}

float clamp_base(const AttributesComponent& c, const Attribute& a, float v) {
    const float lo = bound_value(c, a, false, 0), hi = bound_value(c, a, true, 0);
    return std::clamp(v, std::min(lo, hi), hi);
}

std::uint64_t definition_hash(std::string_view s) { return fnv1a64(s) | 1u; }

// The entity's runtime attributes, created from / synced with its AttributeSet. Null when it has neither.
AttributesComponent* attributes(scene::Scene& scene, Entity e, bool create) {
    ecs::World& w = scene.world();
    if (!w.alive(e)) return nullptr;
    const auto* set = w.get<AttributeSetComponent>(e);
    auto* c = w.get<AttributesComponent>(e);
    if (!c && !set && !create) return nullptr;
    if (!c) {
        c = &w.add<AttributesComponent>(e);
        set = w.get<AttributeSetComponent>(e);
    }
    if (set) {
        const std::uint64_t h = definition_hash(set->definition);
        if (c->definition_hash != h) {
            c->definition_hash = h;
            auto parsed = parse_attributes(set->definition);
            if (!parsed) {
                VICTUS_LOG_WARN("gameplay", "entity '{}': AttributeSet: {}", scene.uuid(e).to_string(), parsed.error().message);
            } else {
                for (Attribute& a : *parsed) {
                    if (Attribute* x = find_attr(*c, a.name)) {
                        x->min = a.min;
                        x->max = a.max;
                        x->min_attribute = a.min_attribute;
                        x->max_attribute = a.max_attribute;
                    } else {
                        c->attributes.push_back(std::move(a));
                    }
                }
                for (Attribute& a : c->attributes) a.value = evaluate(*c, a, 0);
            }
        }
    }
    return c;
}

struct Pending {
    std::string name;
    Value payload;
};

// Recomputes current values and collects change/depletion events.
void refresh(AttributesComponent& c, std::vector<Pending>& out) {
    for (Attribute& a : c.attributes) {
        const float v = evaluate(c, a, 0);
        if (v == a.value) continue;
        const float old = a.value;
        a.value = v;
        Value p = Value::object();
        p.set("attribute", a.name);
        p.set("old", old);
        p.set("value", v);
        out.push_back({"attribute_changed", std::move(p)});
        const float lo = bound_value(c, a, false, 0);
        if (std::isfinite(lo) && v <= lo && old > lo) {
            Value d = Value::object();
            d.set("attribute", a.name);
            out.push_back({"attribute_depleted", std::move(d)});
        }
    }
}

void flush(scene::Scene& scene, Entity e, std::vector<Pending>& pending) {
    for (Pending& p : pending) emit_event(scene, p.name, std::move(p.payload), e, e);
    pending.clear();
}

void change_base(AttributesComponent& c, Attribute& a, ModifierOp op, float m) {
    float v = a.base;
    switch (op) {
    case ModifierOp::Add: v += m; break;
    case ModifierOp::Multiply: v *= m; break;
    case ModifierOp::Override: v = m; break;
    }
    a.base = clamp_base(c, a, v);
}

} // namespace

bool has_attribute(scene::Scene& scene, Entity e, std::string_view name) {
    auto* c = attributes(scene, e, false);
    return c && find_attr(*c, name);
}

std::optional<float> attribute(scene::Scene& scene, Entity e, std::string_view name) {
    auto* c = attributes(scene, e, false);
    const Attribute* a = c ? find_attr(*c, name) : nullptr;
    if (!a) return std::nullopt;
    return evaluate(*c, *a, 0);
}

std::optional<float> attribute_base(scene::Scene& scene, Entity e, std::string_view name) {
    auto* c = attributes(scene, e, false);
    const Attribute* a = c ? find_attr(*c, name) : nullptr;
    if (!a) return std::nullopt;
    return a->base;
}

void set_attribute_base(scene::Scene& scene, Entity e, std::string_view name, float value) {
    if (!attribute_name(name) || !std::isfinite(value)) return;
    auto* c = attributes(scene, e, true);
    if (!c) return;
    Attribute* a = find_attr(*c, name);
    if (!a) {
        Attribute n;
        n.name = std::string(name);
        n.base = value;
        n.value = value;
        c->attributes.push_back(std::move(n));
        a = &c->attributes.back();
    }
    a->base = clamp_base(*c, *a, value);
    std::vector<Pending> pending;
    refresh(*c, pending);
    flush(scene, e, pending);
}

std::optional<float> modify_attribute(scene::Scene& scene, Entity e, std::string_view name, float delta) {
    auto* c = attributes(scene, e, false);
    Attribute* a = c ? find_attr(*c, name) : nullptr;
    if (!a || !std::isfinite(delta)) return std::nullopt;
    change_base(*c, *a, ModifierOp::Add, delta);
    std::vector<Pending> pending;
    refresh(*c, pending);
    const float v = evaluate(*c, *a, 0);
    flush(scene, e, pending);
    return v;
}

std::uint64_t apply_effect(scene::Scene& scene, Entity e, const EffectSpec& spec, Entity source) {
    if (!std::isfinite(spec.magnitude) || !std::isfinite(spec.duration) || !std::isfinite(spec.period)) return 0;
    if (!spec.grant_tag.empty() && !valid_tag(spec.grant_tag)) return 0;
    auto* c = attributes(scene, e, true);
    if (!c) return 0;
    std::vector<Pending> pending;
    std::uint64_t id = 0;
    if (spec.duration == 0.0f) { // instant
        if (Attribute* a = find_attr(*c, spec.attribute)) change_base(*c, *a, spec.op, spec.magnitude);
    } else {
        ActiveEffect* existing = nullptr;
        if (spec.stacking != Stacking::Independent && !spec.name.empty())
            for (ActiveEffect& fx : c->effects)
                if (fx.spec.name == spec.name) existing = &fx;
        if (existing) {
            existing->remaining = spec.duration;
            if (spec.stacking == Stacking::Stack) existing->stacks = std::min(existing->stacks + 1, std::max(1u, spec.max_stacks));
            id = existing->id;
        } else {
            ActiveEffect fx;
            fx.id = c->next_effect++;
            fx.spec = spec;
            fx.remaining = spec.duration;
            fx.next_tick = spec.period;
            fx.source = source.is_null() ? Uuid{} : scene.uuid(source);
            id = fx.id;
            c->effects.push_back(std::move(fx));
        }
    }
    refresh(*c, pending);
    flush(scene, e, pending);
    return id;
}

bool remove_effect(scene::Scene& scene, Entity e, std::uint64_t id) {
    auto* c = attributes(scene, e, false);
    if (!c || std::erase_if(c->effects, [&](const ActiveEffect& fx) { return fx.id == id; }) == 0) return false;
    std::vector<Pending> pending;
    refresh(*c, pending);
    flush(scene, e, pending);
    return true;
}

std::size_t remove_effects(scene::Scene& scene, Entity e, std::string_view name) {
    auto* c = attributes(scene, e, false);
    if (!c) return 0;
    const std::size_t n = std::erase_if(c->effects, [&](const ActiveEffect& fx) { return fx.spec.name == name; });
    std::vector<Pending> pending;
    refresh(*c, pending);
    flush(scene, e, pending);
    return n;
}

const ActiveEffect* find_effect(scene::Scene& scene, Entity e, std::string_view name) {
    auto* c = attributes(scene, e, false);
    if (!c) return nullptr;
    for (const ActiveEffect& fx : c->effects)
        if (fx.spec.name == name) return &fx;
    return nullptr;
}

void update_attributes(scene::Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("gameplay.attributes");
    std::vector<Entity> list;
    scene.world().each<AttributeSetComponent>([&](Entity e, AttributeSetComponent&) { list.push_back(e); });
    scene.world().each<AttributesComponent>([&](Entity e, AttributesComponent&) {
        if (!scene.world().get<AttributeSetComponent>(e)) list.push_back(e);
    });
    std::vector<Pending> pending;
    for (Entity e : list) {
        AttributesComponent* c = attributes(scene, e, false);
        if (!c) continue;
        if (c->mirrored) { // the server applies and expires effects (ADR-0258)
            for (ActiveEffect& fx : c->effects)
                if (fx.spec.duration > 0.0f) fx.remaining = std::max(0.0f, fx.remaining - dt);
            refresh(*c, pending);
            flush(scene, e, pending);
            continue;
        }
        for (std::size_t i = 0; i < c->effects.size();) {
            ActiveEffect& fx = c->effects[i];
            float step = dt;
            if (fx.spec.duration > 0.0f) step = std::min(step, fx.remaining);
            if (fx.spec.period > 0.0f) {
                fx.next_tick -= step;
                for (int n = 0; fx.next_tick <= 1e-6f && n < 16; ++n) {
                    if (Attribute* a = find_attr(*c, fx.spec.attribute))
                        change_base(*c, *a, fx.spec.op, fx.spec.op == ModifierOp::Add ? fx.spec.magnitude * static_cast<float>(fx.stacks) : fx.spec.magnitude);
                    fx.next_tick += fx.spec.period;
                }
            }
            if (fx.spec.duration > 0.0f) {
                fx.remaining -= dt;
                if (fx.remaining <= 1e-6f) {
                    c->effects.erase(c->effects.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
            }
            ++i;
        }
        refresh(*c, pending);
        flush(scene, e, pending);
    }
}

// ---- abilities -----------------------------------------------------------------------------------

std::string_view ability_result_name(AbilityResult r) noexcept {
    switch (r) {
    case AbilityResult::Activated: return "activated";
    case AbilityResult::OnCooldown: return "cooldown";
    case AbilityResult::Blocked: return "blocked";
    case AbilityResult::MissingTags: return "missing_tags";
    case AbilityResult::CannotAfford: return "cannot_afford";
    }
    return "unknown";
}

namespace {
std::string cooldown_name(std::string_view ability) {
    std::string s(ability);
    for (char& ch : s)
        if (!tag_char(ch) || ch == '.') ch = '_';
    return s;
}
} // namespace

AbilityResult activate_ability(scene::Scene& scene, Entity e, const AbilitySpec& spec) {
    for (const std::string& t : split_tags(spec.blocked_tags))
        if (has_tag(scene, e, t)) return AbilityResult::Blocked;
    for (const std::string& t : split_tags(spec.required_tags))
        if (!has_tag(scene, e, t)) return AbilityResult::MissingTags;
    const std::string cd = "cooldown." + cooldown_name(spec.name);
    if (find_effect(scene, e, cd)) return AbilityResult::OnCooldown;
    for (const auto& [attr, amount] : spec.costs) {
        const auto v = attribute(scene, e, attr);
        if (!v || *v < amount) return AbilityResult::CannotAfford;
    }
    for (const auto& [attr, amount] : spec.costs) (void)modify_attribute(scene, e, attr, -amount);
    if (spec.cooldown > 0.0f) {
        EffectSpec fx;
        fx.name = cd;
        fx.duration = spec.cooldown;
        fx.grant_tag = "Cooldown." + cooldown_name(spec.name);
        (void)apply_effect(scene, e, fx);
    }
    Value p = Value::object();
    p.set("ability", spec.name);
    emit_event(scene, "ability_activated", std::move(p), e, e);
    return AbilityResult::Activated;
}

float ability_cooldown(scene::Scene& scene, Entity e, std::string_view name) {
    const ActiveEffect* fx = find_effect(scene, e, "cooldown." + cooldown_name(name));
    return fx ? std::max(0.0f, fx->remaining) : 0.0f;
}

// ---- inventory -----------------------------------------------------------------------------------

namespace {
void inventory_event(scene::Scene& scene, Entity e, std::string_view item, std::int64_t delta, std::uint32_t count) {
    Value p = Value::object();
    p.set("item", item);
    p.set("delta", delta);
    p.set("count", count);
    emit_event(scene, "inventory_changed", std::move(p), e, e);
}
} // namespace

std::uint32_t item_count(scene::Scene& scene, Entity e, std::string_view item) {
    const auto* inv = scene.world().alive(e) ? scene.world().get<InventoryComponent>(e) : nullptr;
    if (!inv) return 0;
    std::uint64_t n = 0;
    for (const ItemStack& s : inv->slots)
        if (s.item == item) n += s.count;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(n, UINT32_MAX));
}

std::uint32_t add_item(scene::Scene& scene, Entity e, std::string_view item, std::uint32_t count, std::uint32_t max_stack) {
    if (!scene.world().alive(e) || item.empty() || count == 0) return 0;
    auto* inv = scene.world().get<InventoryComponent>(e);
    if (!inv) inv = &scene.world().add<InventoryComponent>(e);
    const std::uint32_t stack = std::max(1u, max_stack ? max_stack : inv->max_stack);
    std::uint32_t left = count;
    for (ItemStack& s : inv->slots) {
        if (left == 0) break;
        if (s.item != item || s.count >= stack) continue;
        const std::uint32_t put = std::min(left, stack - s.count);
        s.count += put;
        left -= put;
    }
    while (left > 0 && inv->slots.size() < inv->capacity) {
        const std::uint32_t put = std::min(left, stack);
        inv->slots.push_back({std::string(item), put});
        left -= put;
    }
    const std::uint32_t added = count - left;
    if (added > 0) inventory_event(scene, e, item, added, item_count(scene, e, item));
    return added;
}

std::uint32_t remove_item(scene::Scene& scene, Entity e, std::string_view item, std::uint32_t count) {
    auto* inv = scene.world().alive(e) ? scene.world().get<InventoryComponent>(e) : nullptr;
    if (!inv || count == 0) return 0;
    std::uint32_t left = count;
    for (std::size_t i = inv->slots.size(); i-- > 0 && left > 0;) {
        ItemStack& s = inv->slots[i];
        if (s.item != item) continue;
        const std::uint32_t take = std::min(left, s.count);
        s.count -= take;
        left -= take;
        if (s.count == 0) inv->slots.erase(inv->slots.begin() + static_cast<std::ptrdiff_t>(i));
    }
    const std::uint32_t removed = count - left;
    if (removed > 0) inventory_event(scene, e, item, -static_cast<std::int64_t>(removed), item_count(scene, e, item));
    return removed;
}

std::uint32_t transfer_item(scene::Scene& scene, Entity from, Entity to, std::string_view item, std::uint32_t count) {
    const std::uint32_t have = std::min(count, item_count(scene, from, item));
    if (have == 0 || from == to) return 0;
    const std::uint32_t moved = add_item(scene, to, item, have);
    if (moved > 0) (void)remove_item(scene, from, item, moved);
    return moved;
}

// ---- registration --------------------------------------------------------------------------------

namespace {

Value float_or_null(float v) { return std::isfinite(v) ? Value(v) : Value(); }
float float_from(const Value& v, float fallback) { return v.is_number() ? v.as_float(fallback) : fallback; }
std::string hex(std::uint64_t v) { return std::format("{:016x}", v); }
std::uint64_t from_hex(const Value& v) { return v.is_string() ? std::strtoull(v.as_string().c_str(), nullptr, 16) : 0; }

constexpr const char* kOps[] = {"add", "multiply", "override"};
constexpr const char* kStacking[] = {"refresh", "stack", "independent"};
template <std::size_t N>
std::uint8_t index_of(const char* const (&names)[N], const Value& v) {
    for (std::size_t i = 0; i < N; ++i)
        if (v.as_string_or("") == names[i]) return static_cast<std::uint8_t>(i);
    return 0;
}

} // namespace

json::Value replication_key(std::string_view component, const json::Value& state) {
    if (component != "Attributes" || !state.is_object()) return state;
    Value key = Value::object();
    for (const auto& [k, v] : state.as_object()) {
        if (k != "effects" || !v.is_array()) {
            key.set(k, v);
            continue;
        }
        Value fxs = Value::array();
        for (const Value& fx : v.as_array()) {
            if (!fx.is_object()) continue;
            Value f = Value::object();
            for (const auto& [fk, fv] : fx.as_object())
                if (fk != "remaining" && fk != "next_tick") f.set(fk, fv);
            fxs.push(std::move(f));
        }
        key.set(k, std::move(fxs));
    }
    return key;
}

Status apply_replicated(scene::Scene& scene, Entity e, std::string_view component, const json::Value& state) {
    ecs::World& w = scene.world();
    if (!w.alive(e)) return make_error(ErrorCode::InvalidArgument, "apply_replicated: entity is not alive");
    const scene::ComponentSerializer* ser = scene::SerializerRegistry::instance().by_name(component);
    if (!ser) return make_error(ErrorCode::NotFound, "apply_replicated: unknown component '" + std::string(component) + "'");
    if (state.is_null()) {
        w.remove_raw(e, ser->id);
        return {};
    }
    if (!state.is_object()) return make_error(ErrorCode::InvalidArgument, "apply_replicated: state must be an object");
    // Values before the update, so the change events report what this client saw.
    std::vector<std::pair<std::string, float>> old_values;
    std::vector<std::pair<std::string, std::uint32_t>> old_items;
    if (component == "Attributes") {
        if (const auto* a = w.get<AttributesComponent>(e))
            for (const Attribute& x : a->attributes) old_values.emplace_back(x.name, x.value);
    } else if (component == "Inventory") {
        if (const auto* inv = w.get<InventoryComponent>(e))
            for (const ItemStack& s : inv->slots) {
                auto it = std::find_if(old_items.begin(), old_items.end(), [&](const auto& p) { return p.first == s.item; });
                if (it == old_items.end()) old_items.emplace_back(s.item, s.count);
                else it->second += s.count;
            }
    }
    void* raw = w.get_raw(e, ser->id);
    const bool fresh = raw == nullptr;
    if (fresh) raw = w.add_raw(e, ser->id);
    if (Status st = ser->load(raw, state); !st.ok()) {
        if (fresh) w.remove_raw(e, ser->id);
        return st;
    }
    if (component == "Attributes") {
        auto& a = *static_cast<AttributesComponent*>(raw);
        a.mirrored = true;
        // Keep the previous values: the next update_attributes refresh emits the changes.
        for (Attribute& x : a.attributes)
            for (const auto& [name, v] : old_values)
                if (name == x.name) x.value = v;
    } else if (component == "Inventory") {
        std::vector<std::pair<std::string, std::uint32_t>> now;
        for (const ItemStack& s : static_cast<InventoryComponent*>(raw)->slots) {
            auto it = std::find_if(now.begin(), now.end(), [&](const auto& p) { return p.first == s.item; });
            if (it == now.end()) now.emplace_back(s.item, s.count);
            else it->second += s.count;
        }
        const auto count_in = [](const auto& list, const std::string& item) -> std::int64_t {
            for (const auto& [name, n] : list)
                if (name == item) return n;
            return 0;
        };
        const auto report = [&](const std::string& item) {
            const std::int64_t before = count_in(old_items, item), after = count_in(now, item);
            if (before == after) return;
            Value p = Value::object();
            p.set("item", item);
            p.set("delta", static_cast<double>(after - before));
            p.set("count", static_cast<double>(after));
            emit_event(scene, "inventory_changed", std::move(p), e, e);
        };
        for (const auto& [item, n] : now) report(item);
        for (const auto& [item, n] : old_items)
            if (count_in(now, item) == 0) report(item);
    }
    return {};
}

void register_framework_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<TagsComponent>("Tags", "Gameplay")
            .field(&TagsComponent::tags, "tags", {.tooltip = "Space-separated; dots form a hierarchy (Status.Burning matches Status)"})
            .commit();
        scene::Schema<AttributeSetComponent>("AttributeSet", "Gameplay")
            .field(&AttributeSetComponent::definition, "definition",
                   {.tooltip = "name = value [min..max]; separated by ';'. Bounds may name other attributes: health = 100 [0..max_health]"})
            .commit();
        auto& r = scene::SerializerRegistry::instance();
        r.add<TimersComponent>(
            "Timers",
            [](const TimersComponent& c) {
                Value list = Value::array();
                for (const Timer& t : c.timers) {
                    Value v = Value::object();
                    v.set("name", t.name);
                    v.set("remaining", t.remaining);
                    v.set("interval", t.interval);
                    v.set("repeat", t.repeat);
                    v.set("paused", t.paused);
                    list.push(std::move(v));
                }
                Value v = Value::object();
                v.set("timers", std::move(list));
                return v;
            },
            [](TimersComponent& c, const Value& v) -> Status {
                c.timers.clear();
                if (const Value* list = v.find("timers"); list && list->is_array())
                    for (const Value& t : list->as_array()) {
                        if (!t["name"].is_string()) continue;
                        c.timers.push_back({t["name"].as_string(), t["remaining"].as_float(0), t["interval"].as_float(0),
                                            t["repeat"].as_bool(false), t["paused"].as_bool(false)});
                    }
                return {};
            });
        r.add<AttributesComponent>(
            "Attributes",
            [](const AttributesComponent& c) {
                Value attrs = Value::array();
                for (const Attribute& a : c.attributes) {
                    Value v = Value::object();
                    v.set("name", a.name);
                    v.set("base", a.base);
                    v.set("value", a.value);
                    v.set("min", float_or_null(a.min));
                    v.set("max", float_or_null(a.max));
                    if (!a.min_attribute.empty()) v.set("min_attribute", a.min_attribute);
                    if (!a.max_attribute.empty()) v.set("max_attribute", a.max_attribute);
                    attrs.push(std::move(v));
                }
                Value fxs = Value::array();
                for (const ActiveEffect& fx : c.effects) {
                    Value v = Value::object();
                    v.set("id", fx.id);
                    v.set("name", fx.spec.name);
                    v.set("attribute", fx.spec.attribute);
                    v.set("op", kOps[static_cast<int>(fx.spec.op)]);
                    v.set("magnitude", fx.spec.magnitude);
                    v.set("duration", fx.spec.duration);
                    v.set("period", fx.spec.period);
                    v.set("grant_tag", fx.spec.grant_tag);
                    v.set("stacking", kStacking[static_cast<int>(fx.spec.stacking)]);
                    v.set("max_stacks", fx.spec.max_stacks);
                    v.set("remaining", fx.remaining);
                    v.set("next_tick", fx.next_tick);
                    v.set("stacks", fx.stacks);
                    if (!fx.source.is_nil()) v.set("source", fx.source.to_string());
                    fxs.push(std::move(v));
                }
                Value v = Value::object();
                v.set("attributes", std::move(attrs));
                v.set("effects", std::move(fxs));
                v.set("next_effect", c.next_effect);
                v.set("definition_hash", hex(c.definition_hash));
                return v;
            },
            [](AttributesComponent& c, const Value& v) -> Status {
                c = {};
                constexpr float inf = std::numeric_limits<float>::infinity();
                if (const Value* list = v.find("attributes"); list && list->is_array())
                    for (const Value& a : list->as_array()) {
                        Attribute x;
                        x.name = a["name"].as_string_or("");
                        if (!attribute_name(x.name)) continue;
                        x.base = a["base"].as_float(0);
                        x.value = a["value"].as_float(x.base);
                        x.min = float_from(a["min"], -inf);
                        x.max = float_from(a["max"], inf);
                        x.min_attribute = a["min_attribute"].as_string_or("");
                        x.max_attribute = a["max_attribute"].as_string_or("");
                        c.attributes.push_back(std::move(x));
                    }
                if (const Value* list = v.find("effects"); list && list->is_array())
                    for (const Value& f : list->as_array()) {
                        ActiveEffect fx;
                        fx.id = static_cast<std::uint64_t>(f["id"].as_int(0));
                        fx.spec.name = f["name"].as_string_or("");
                        fx.spec.attribute = f["attribute"].as_string_or("");
                        fx.spec.op = static_cast<ModifierOp>(index_of(kOps, f["op"]));
                        fx.spec.magnitude = f["magnitude"].as_float(0);
                        fx.spec.duration = f["duration"].as_float(-1);
                        fx.spec.period = f["period"].as_float(0);
                        fx.spec.grant_tag = f["grant_tag"].as_string_or("");
                        if (!fx.spec.grant_tag.empty() && !valid_tag(fx.spec.grant_tag)) fx.spec.grant_tag.clear();
                        fx.spec.stacking = static_cast<Stacking>(index_of(kStacking, f["stacking"]));
                        fx.spec.max_stacks = static_cast<std::uint32_t>(std::clamp<std::int64_t>(f["max_stacks"].as_int(1), 1, 1 << 20));
                        fx.remaining = f["remaining"].as_float(0);
                        fx.next_tick = f["next_tick"].as_float(0);
                        fx.stacks = static_cast<std::uint32_t>(std::clamp<std::int64_t>(f["stacks"].as_int(1), 1, 1 << 20));
                        if (const Value* s = f.find("source"); s && s->is_string()) (void)Uuid::parse(s->as_string(), fx.source);
                        c.next_effect = std::max(c.next_effect, fx.id + 1);
                        c.effects.push_back(std::move(fx));
                    }
                c.next_effect = std::max<std::uint64_t>(c.next_effect, static_cast<std::uint64_t>(v["next_effect"].as_int(1)));
                c.definition_hash = from_hex(v["definition_hash"]);
                return {};
            });
        r.add<InventoryComponent>(
            "Inventory",
            [](const InventoryComponent& c) {
                Value slots = Value::array();
                for (const ItemStack& s : c.slots) {
                    Value v = Value::object();
                    v.set("item", s.item);
                    v.set("count", s.count);
                    slots.push(std::move(v));
                }
                Value v = Value::object();
                v.set("capacity", c.capacity);
                v.set("max_stack", c.max_stack);
                v.set("slots", std::move(slots));
                return v;
            },
            [](InventoryComponent& c, const Value& v) -> Status {
                c = {};
                c.capacity = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["capacity"].as_int(20), 0, 1 << 16));
                c.max_stack = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["max_stack"].as_int(99), 1, 1 << 30));
                if (const Value* list = v.find("slots"); list && list->is_array())
                    for (const Value& s : list->as_array()) {
                        const auto n = static_cast<std::uint32_t>(std::clamp<std::int64_t>(s["count"].as_int(0), 0, 1 << 30));
                        if (s["item"].is_string() && !s["item"].as_string().empty() && n > 0) c.slots.push_back({s["item"].as_string(), n});
                    }
                return {};
            });
    });
}

} // namespace victus::gameplay
