// SPDX-License-Identifier: Apache-2.0
// Gameplay framework (ADR-0094): tags, events, timers, attributes/effects, abilities, inventory.
#include "victus/gameplay/framework.hpp"
#include "victus/gameplay/gameplay.hpp"

#include "doctest.h"

#include <string>
#include <vector>

using namespace victus;
using namespace victus::gameplay;
using scene::Entity;
using scene::Scene;

namespace {

Entity actor(Scene& s, const char* name, const char* attrs = nullptr) {
    register_components();
    const Entity e = s.create(name);
    if (attrs) s.world().add<AttributeSetComponent>(e).definition = attrs;
    return e;
}

float attr(Scene& s, Entity e, const char* n) {
    const auto v = attribute(s, e, n);
    REQUIRE_MESSAGE(v.has_value(), n);
    return *v;
}

std::vector<std::string> names(Scene& s) {
    std::vector<std::string> out;
    for (const auto& ev : recent_events(s)) out.push_back(ev.name);
    return out;
}

} // namespace

TEST_CASE("gameplay tags: hierarchical matching, add/remove and queries") {
    CHECK(valid_tag("Status.Burning"));
    CHECK(valid_tag("team_red-2"));
    for (const char* bad : {"", ".a", "a.", "a..b", "with space", "a,b"}) CHECK_MESSAGE(!valid_tag(bad), bad);
    CHECK(tag_matches("Status.Burning", "Status"));
    CHECK(tag_matches("Status.Burning", "Status.Burning"));
    CHECK_FALSE(tag_matches("StatusX", "Status"));
    CHECK_FALSE(tag_matches("Status", "Status.Burning"));
    CHECK(split_tags(" a, b;c  d ").size() == 4);

    Scene s;
    const Entity a = actor(s, "a"), b = actor(s, "b"), c = actor(s, "c");
    CHECK(add_tag(s, a, "Enemy.Orc"));
    CHECK_FALSE(add_tag(s, a, "Enemy.Orc")); // already present
    CHECK(add_tag(s, b, "Enemy.Goblin"));
    CHECK(add_tag(s, c, "Friend"));
    CHECK(has_tag(s, a, "Enemy"));
    CHECK_FALSE(has_tag(s, c, "Enemy"));
    const auto enemies = find_tagged(s, "Enemy");
    REQUIRE(enemies.size() == 2);
    CHECK(enemies[0] == a);
    CHECK(enemies[1] == b);
    CHECK(remove_tag(s, a, "Enemy.Orc"));
    CHECK_FALSE(has_tag(s, a, "Enemy"));
    CHECK(find_tagged(s, "Enemy").size() == 1);
}

TEST_CASE("gameplay events: listeners run immediately and the queue keeps two ticks") {
    Scene s;
    const Entity a = actor(s, "a");
    int heard = 0;
    std::string last;
    const auto handle = listen(s, "hit", [&](Scene&, const GameplayEvent& ev) {
        ++heard;
        last = ev.payload["part"].as_string_or("");
    });
    int all = 0;
    (void)listen(s, "", [&](Scene&, const GameplayEvent&) { ++all; });
    json::Value p = json::Value::object();
    p.set("part", "head");
    const auto id1 = emit_event(s, "hit", p, a);
    const auto id2 = emit_event(s, "other", {}, a);
    CHECK(id2 > id1);
    CHECK(heard == 1);
    CHECK(last == "head");
    CHECK(all == 2);
    unlisten(s, handle);
    (void)emit_event(s, "hit");
    CHECK(heard == 1);
    CHECK(recent_events(s).size() == 3);
    begin_event_tick(s);
    CHECK(recent_events(s).size() == 3); // still visible the next tick
    begin_event_tick(s);
    begin_event_tick(s);
    CHECK(recent_events(s).empty());

    // Re-entrant emits are bounded instead of recursing forever.
    int depth = 0;
    (void)listen(s, "loop", [&](Scene& sc, const GameplayEvent&) {
        ++depth;
        (void)emit_event(sc, "loop");
    });
    (void)emit_event(s, "loop");
    CHECK(depth > 1);
    CHECK(depth <= 17);
}

TEST_CASE("gameplay timers: one-shot, repeating, pause and save round trip") {
    Scene s;
    const Entity a = actor(s, "a");
    start_timer(s, a, "boom", 1.0f);
    start_timer(s, a, "tick", 0.25f, true);
    update_timers(s, 0.5f);
    CHECK(timer_remaining(s, a, "boom").value() == doctest::Approx(0.5f));
    int ticks = 0;
    for (const auto& n : names(s)) ticks += n == "tick";
    CHECK(ticks == 2);
    CHECK(pause_timer(s, a, "boom", true));
    update_timers(s, 2.0f);
    CHECK(timer_remaining(s, a, "boom").value() == doctest::Approx(0.5f));
    CHECK(pause_timer(s, a, "boom", false));

    const json::Value doc = s.save();
    Scene r;
    register_components();
    REQUIRE(r.load(doc).ok());
    const Entity ra = r.find_by_name("a");
    REQUIRE(!ra.is_null());
    CHECK(timer_remaining(r, ra, "boom").value() == doctest::Approx(0.5f));
    update_timers(r, 0.6f);
    CHECK_FALSE(timer_remaining(r, ra, "boom").has_value());
    bool fired = false;
    for (const auto& ev : recent_events(r))
        if (ev.name == "boom") fired = ev.target == ra && ev.payload["timer"].as_bool();
    CHECK(fired);
    CHECK(timer_remaining(r, ra, "tick").has_value()); // repeating timers keep running
    CHECK(stop_timer(r, ra, "tick"));
    CHECK_FALSE(stop_timer(r, ra, "tick"));
}

TEST_CASE("gameplay attributes: definitions, bounds by other attributes and errors") {
    auto ok = parse_attributes("health = 80 [0..max_health]; max_health = 100\nspeed = 5 [1..10]");
    REQUIRE(ok.ok());
    REQUIRE(ok->size() == 3);
    CHECK((*ok)[0].max_attribute == "max_health");
    for (const char* bad : {"health", "health = x", "health = 1 [0..", "= 3", "a = 1; a = 2"})
        CHECK_MESSAGE(!parse_attributes(bad).ok(), bad);

    Scene s;
    const Entity e = actor(s, "hero", "health = 80 [0..max_health]; max_health = 100; speed = 5 [1..10]");
    CHECK(has_attribute(s, e, "health"));
    CHECK_FALSE(has_attribute(s, e, "mana"));
    CHECK(attr(s, e, "health") == doctest::Approx(80));
    CHECK(modify_attribute(s, e, "health", 50).value() == doctest::Approx(100)); // clamped by max_health
    set_attribute_base(s, e, "max_health", 60);
    CHECK(attr(s, e, "health") == doctest::Approx(60));
    set_attribute_base(s, e, "speed", 50);
    CHECK(attr(s, e, "speed") == doctest::Approx(10));
}

TEST_CASE("gameplay effects: modifiers, stacking, periodic damage, granted tags and expiry") {
    Scene s;
    const Entity e = actor(s, "hero", "health = 100 [0..100]; speed = 5");
    // Temporary multiplier with stacking.
    EffectSpec haste{.name = "haste", .attribute = "speed", .op = ModifierOp::Multiply, .magnitude = 2,
                     .duration = 1, .grant_tag = "Status.Haste", .stacking = Stacking::Stack, .max_stacks = 2};
    (void)apply_effect(s, e, haste);
    CHECK(attr(s, e, "speed") == doctest::Approx(10));
    (void)apply_effect(s, e, haste);
    (void)apply_effect(s, e, haste); // capped at 2 stacks
    CHECK(attr(s, e, "speed") == doctest::Approx(20));
    CHECK(find_effect(s, e, "haste")->stacks == 2);
    CHECK(has_tag(s, e, "Status"));

    // Damage over time: -10 every 0.25 s for 1 s.
    EffectSpec burn{.name = "burn", .attribute = "health", .magnitude = -10, .duration = 1, .period = 0.25f,
                    .grant_tag = "Status.Burning"};
    (void)apply_effect(s, e, burn);
    for (int i = 0; i < 60; ++i) update_attributes(s, 1.0f / 60.0f);
    update_attributes(s, 0.05f);
    CHECK(attr(s, e, "health") == doctest::Approx(60).epsilon(0.01));
    CHECK_FALSE(has_tag(s, e, "Status.Burning"));
    CHECK(attr(s, e, "speed") == doctest::Approx(5)); // haste expired
    CHECK(find_effect(s, e, "haste") == nullptr);

    // Instant effects change the base; overrides win while active; removal restores.
    (void)apply_effect(s, e, {.name = "heal", .attribute = "health", .magnitude = 25});
    CHECK(attribute_base(s, e, "health").value() == doctest::Approx(85));
    const auto root = apply_effect(s, e, {.name = "root", .attribute = "speed", .op = ModifierOp::Override, .magnitude = 0, .duration = -1});
    CHECK(attr(s, e, "speed") == doctest::Approx(0));
    CHECK(remove_effect(s, e, root));
    CHECK(attr(s, e, "speed") == doctest::Approx(5));

    // Depletion fires once when the value reaches the minimum.
    begin_event_tick(s);
    begin_event_tick(s);
    (void)apply_effect(s, e, {.name = "smite", .attribute = "health", .magnitude = -500});
    update_attributes(s, 0.01f);
    int depleted = 0;
    for (const auto& n : names(s)) depleted += n == "attribute_depleted";
    CHECK(depleted == 1);
    CHECK(attr(s, e, "health") == doctest::Approx(0));

    // Effects survive save/load.
    (void)apply_effect(s, e, haste);
    Scene r;
    REQUIRE(r.load(s.save()).ok());
    const Entity re = r.find_by_name("hero");
    REQUIRE(find_effect(r, re, "haste") != nullptr);
    CHECK(attr(r, re, "speed") == doctest::Approx(10));
    CHECK(has_tag(r, re, "Status.Haste"));
}

TEST_CASE("gameplay abilities: tags, costs and cooldowns gate activation") {
    Scene s;
    const Entity e = actor(s, "hero", "stamina = 30 [0..100]");
    AbilitySpec dash{.name = "dash", .cooldown = 1, .costs = {{"stamina", 20.0f}}, .required_tags = "", .blocked_tags = "Status.Stunned"};
    CHECK(activate_ability(s, e, dash) == AbilityResult::Activated);
    CHECK(attr(s, e, "stamina") == doctest::Approx(10));
    CHECK(ability_cooldown(s, e, "dash") == doctest::Approx(1));
    CHECK(has_tag(s, e, "Cooldown.dash"));
    CHECK(activate_ability(s, e, dash) == AbilityResult::OnCooldown);
    for (int i = 0; i < 70; ++i) update_attributes(s, 1.0f / 60.0f);
    CHECK(ability_cooldown(s, e, "dash") == doctest::Approx(0));
    CHECK(activate_ability(s, e, dash) == AbilityResult::CannotAfford);
    set_attribute_base(s, e, "stamina", 100);
    (void)add_tag(s, e, "Status.Stunned");
    CHECK(activate_ability(s, e, dash) == AbilityResult::Blocked);
    (void)remove_tag(s, e, "Status.Stunned");
    AbilitySpec smash{.name = "smash", .required_tags = "Weapon.Hammer"};
    CHECK(activate_ability(s, e, smash) == AbilityResult::MissingTags);
    CHECK(ability_result_name(AbilityResult::MissingTags) == "missing_tags");
    bool activated = false;
    for (const auto& ev : recent_events(s)) activated |= ev.name == "ability_activated";
    CHECK(activated);
}

TEST_CASE("gameplay inventory: stacks, capacity, removal order, transfer and save") {
    Scene s;
    const Entity a = actor(s, "a"), b = actor(s, "b");
    auto& inv = s.world().add<InventoryComponent>(a);
    inv.capacity = 3;
    inv.max_stack = 10;
    CHECK(add_item(s, a, "coin", 25) == 25); // 10 + 10 + 5
    CHECK(add_item(s, a, "coin", 10) == 5);  // the last stack fills, no slots left
    CHECK(add_item(s, a, "gem", 1) == 0);
    CHECK(item_count(s, a, "coin") == 30);
    CHECK(remove_item(s, a, "coin", 12) == 12);
    CHECK(item_count(s, a, "coin") == 18);
    CHECK(add_item(s, a, "gem", 2) == 2);
    CHECK(transfer_item(s, a, b, "coin", 100) == 18);
    CHECK(item_count(s, a, "coin") == 0);
    CHECK(item_count(s, b, "coin") == 18);
    Scene r;
    REQUIRE(r.load(s.save()).ok());
    CHECK(item_count(r, r.find_by_name("b"), "coin") == 18);
    CHECK(item_count(r, r.find_by_name("a"), "gem") == 2);
    CHECK(r.world().get<InventoryComponent>(r.find_by_name("a"))->capacity == 3);
}

TEST_CASE("gameplay framework: builtin systems drive timers and effects") {
    Scene s;
    const Entity e = actor(s, "hero", "health = 10");
    Simulation sim;
    add_builtin_systems(sim);
    start_timer(s, e, "done", 0.5f);
    (void)apply_effect(s, e, {.name = "regen", .attribute = "health", .magnitude = 1, .duration = 1, .period = 0.1f});
    for (int i = 0; i < 60; ++i) sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(timer_remaining(s, e, "done").has_value());
    CHECK(attr(s, e, "health") == doctest::Approx(20).epsilon(0.02));
}
