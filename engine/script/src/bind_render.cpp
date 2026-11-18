// SPDX-License-Identifier: Apache-2.0
// Script bindings: In-game UI (ui.*), materials, colours, sprites, tilemaps, animation and particle entity methods.
#include "script_internal.hpp"

namespace victus::script::detail {


// ---- In-game UI ---------------------------------------------------------------------------------

// entity:set_text(value): sets the entity's UiText (numbers and booleans are converted).
int entity_set_text(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    size_t len = 0;
    const char* text = luaL_tolstring(L, 2, &len);
    auto* t = s.world().get<gameui::UiTextComponent>(e);
    if (!t) luaL_error(L, "entity has no UiText component");
    t->text.assign(text, len);
    t->key.clear(); // literal text from a script replaces any localization key
    return 0;
}

// entity:scroll_to(x, y) scrolls a UiScroll view (canvas units, clamped to its content).
int entity_scroll_to(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto x = static_cast<float>(luaL_checknumber(L, 2));
    const auto y = static_cast<float>(luaL_checknumber(L, 3));
    if (!gameui::scroll_to(s, e, {x, y})) luaL_error(L, "entity has no UiScroll component");
    return 0;
}

// entity:scroll() -> x, y, max_x, max_y (canvas units; max is 0 on an axis that cannot scroll).
int entity_scroll(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* sc = s.world().get<gameui::UiScrollComponent>(e);
    if (!sc) luaL_error(L, "entity has no UiScroll component");
    const auto* st = s.world().get<gameui::UiStateComponent>(e);
    lua_pushnumber(L, sc->offset.x);
    lua_pushnumber(L, sc->offset.y);
    lua_pushnumber(L, st ? st->scroll_max.x : 0.0);
    lua_pushnumber(L, st ? st->scroll_max.y : 0.0);
    return 4;
}

// entity:set_text_key(key?) -> shows the key's translation (loc) and follows locale changes;
// nil or "" goes back to the literal text.
int entity_set_text_key(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* t = s.world().get<gameui::UiTextComponent>(e);
    if (!t) luaL_error(L, "entity has no UiText component");
    t->key = lua_isnoneornil(L, 2) ? std::string() : std::string(luaL_checkstring(L, 2));
    return 0;
}

int entity_text(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* t = s.world().get<gameui::UiTextComponent>(e);
    if (t) lua_pushlstring(L, t->text.data(), t->text.size());
    else lua_pushnil(L);
    return 1;
}

// entity:set_visible(bool): the UiCanvas when the entity is a canvas root, else its UiRect, else
// its MeshRenderer.
int entity_set_visible(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const bool v = lua_toboolean(L, 2) != 0;
    if (auto* c = s.world().get<gameui::UiCanvasComponent>(e)) c->visible = v;
    else if (auto* r = s.world().get<gameui::UiRectComponent>(e)) r->visible = v;
    else if (auto* m = s.world().get<scene::MeshRendererComponent>(e)) m->visible = v;
    else luaL_error(L, "entity has no UiRect, UiCanvas or MeshRenderer component");
    return 0;
}

int entity_visible(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (const auto* c = s.world().get<gameui::UiCanvasComponent>(e)) lua_pushboolean(L, c->visible);
    else if (const auto* r = s.world().get<gameui::UiRectComponent>(e)) lua_pushboolean(L, r->visible);
    else if (const auto* m = s.world().get<scene::MeshRendererComponent>(e)) lua_pushboolean(L, m->visible);
    else lua_pushnil(L);
    return 1;
}

// ---- Rendering ----------------------------------------------------------------------------------

constexpr const char* kPrimitives[] = {"cube", "sphere", "plane", "cylinder"};


bool is_primitive(std::string_view name) {
    return std::ranges::any_of(kPrimitives, [&](const char* p) { return name == p; });
}


// Resolves an asset path (or a UUID string) to the asset's UUID; raises a Luau error on failure.
Uuid resolve_asset(lua_State* L, std::string_view path, const char* fn) {
    auto& h = host_of(L);
    const std::string key(path);
    if (const auto it = h.asset_uuids.find(key); it != h.asset_uuids.end()) return it->second;
    Uuid id;
    if (!Uuid::parse(path, id)) {
        if (h.options.asset_resolver) {
            auto r = h.options.asset_resolver(path);
            if (!r) luaL_error(L, "%s: cannot resolve '%s': %s", fn, key.c_str(), r.error().message.c_str());
            id = *r;
        } else {
            auto file = fs::resolve_under(h.options.assets_root, path);
            if (!file) luaL_error(L, "%s: cannot resolve '%s': %s", fn, key.c_str(), file.error().message.c_str());
            sfs::path meta = *file;
            meta += ".meta";
            bool found = false;
            if (auto text = fs::read_text(meta, 1u << 20))
                if (auto doc = json::parse(*text); doc && (*doc)["uuid"].is_string()) found = Uuid::parse((*doc)["uuid"].as_string(), id);
            if (!found) luaL_error(L, "%s: '%s' is not an imported asset (no UUID in its .meta file)", fn, key.c_str());
        }
        if (id.is_nil()) luaL_error(L, "%s: '%s' has no asset UUID", fn, key.c_str());
    }
    h.asset_uuids.emplace(key, id);
    h.asset_paths.emplace(id, key);
    return id;
}


void push_asset(lua_State* L, const Uuid& id) {
    if (id.is_nil()) {
        lua_pushnil(L);
        return;
    }
    auto& h = host_of(L);
    const auto it = h.asset_paths.find(id);
    const std::string text = it != h.asset_paths.end() ? it->second : id.to_string();
    lua_pushlstring(L, text.data(), text.size());
}


scene::MeshRendererComponent& check_mesh_renderer(lua_State* L, scene::Scene& s, Entity e) {
    auto* m = s.world().get<scene::MeshRendererComponent>(e);
    if (!m) luaL_error(L, "entity has no MeshRenderer component");
    return *m;
}


// entity:set_material(path | uuid | nil): the MeshRenderer's .vmat material (nil: the mesh's own).
int entity_set_material(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = check_mesh_renderer(L, s, e);
    if (lua_isnoneornil(L, 2)) {
        m.material_asset = {};
        return 0;
    }
    const std::string_view path = luaL_checkstring(L, 2);
    Uuid tmp;
    if (!Uuid::parse(path, tmp) && !path.ends_with(".vmat")) luaL_error(L, "set_material: '%s' is not a .vmat material", std::string(path).c_str());
    m.material_asset = resolve_asset(L, path, "set_material");
    return 0;
}

// entity:material() -> path (or UUID string when set outside scripts) | nil
int entity_material(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* m = s.world().get<scene::MeshRendererComponent>(e);
    if (!m) lua_pushnil(L);
    else push_asset(L, m->material_asset);
    return 1;
}

// ---- tilemaps ------------------------------------------------------------------------------
tilemap::TilemapComponent& tilemap_of(lua_State* L, scene::Scene& s, Entity e) {
    auto* m = s.world().get<tilemap::TilemapComponent>(e);
    if (!m) luaL_error(L, "entity has no Tilemap component");
    return *m;
}

std::int32_t check_cell(lua_State* L, int idx) {
    const double v = luaL_checknumber(L, idx);
    if (!(v >= -tilemap::kMaxCoordinate && v <= tilemap::kMaxCoordinate))
        luaL_error(L, "tile coordinate %f is out of range", v);
    return static_cast<std::int32_t>(std::floor(v));
}

std::int32_t opt_tile(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return -1;
    const double v = luaL_checknumber(L, idx);
    if (!(v >= -1 && v < tilemap::kMaxTiles)) luaL_error(L, "tile index %f is out of range", v);
    return static_cast<std::int32_t>(v);
}

// entity:set_tile(x, y, tile | nil, flip_x?, flip_y?): paints (or erases with nil) one cell.
int entity_set_tile(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = tilemap_of(L, s, e);
    const std::int32_t x = check_cell(L, 2), y = check_cell(L, 3);
    tilemap::set_tile(m, x, y, opt_tile(L, 4), lua_toboolean(L, 5) != 0, lua_toboolean(L, 6) != 0);
    return 0;
}

// entity:tile(x, y) -> tile index or nil, flip_x, flip_y
int entity_tile(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto& m = tilemap_of(L, s, e);
    const tilemap::Cell* c = m.cell(check_cell(L, 2), check_cell(L, 3));
    if (!c || c->tile < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, c->tile);
    lua_pushboolean(L, c->flip_x);
    lua_pushboolean(L, c->flip_y);
    return 3;
}

// entity:fill_tiles(x0, y0, x1, y1, tile | nil) -> cells changed (inclusive rectangle).
int entity_fill_tiles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = tilemap_of(L, s, e);
    const std::int32_t x0 = check_cell(L, 2), y0 = check_cell(L, 3), x1 = check_cell(L, 4), y1 = check_cell(L, 5);
    const std::uint64_t area = static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(x1) - x0) + 1) *
                               static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(y1) - y0) + 1);
    if (area > tilemap::kMaxFillCells) luaL_error(L, "fill_tiles: rectangle has more than %d cells", static_cast<int>(tilemap::kMaxFillCells));
    lua_pushnumber(L, static_cast<double>(tilemap::fill_tiles(m, x0, y0, x1, y1, opt_tile(L, 6))));
    return 1;
}

// entity:clear_tiles()
int entity_clear_tiles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    tilemap::clear_tiles(tilemap_of(L, s, e));
    return 0;
}

// entity:paint_terrain(x, y, name | nil): autotiled painting with the tile set's terrain rules.
int entity_paint_terrain(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    tilemap_of(L, s, e);
    const std::int32_t x = check_cell(L, 2), y = check_cell(L, 3);
    const auto* rt = s.world().get<tilemap::TilemapRuntime>(e);
    if (!rt || !rt->tileset) luaL_error(L, "paint_terrain: the tilemap's tile set is not loaded");
    std::int32_t terrain = -1;
    if (!lua_isnoneornil(L, 4)) {
        const char* name = luaL_checkstring(L, 4);
        terrain = rt->tileset->terrain_index(name);
        if (terrain < 0) luaL_error(L, "paint_terrain: the tile set has no terrain '%s'", name);
    }
    const auto ts = rt->tileset; // keep alive while painting
    tilemap::paint_terrain(tilemap_of(L, s, e), *ts, x, y, terrain);
    return 0;
}

// entity:world_to_tile(point) -> x, y of the cell containing a world position.
int entity_world_to_tile(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto& m = tilemap_of(L, s, e);
    const math::Vec3 local = math::inverse(s.world_matrix(e)).transform_point(check_vec3(L, 2));
    lua_pushinteger(L, static_cast<int>(std::floor(local.x / std::max(m.cell_size.x, 1e-6f))));
    lua_pushinteger(L, static_cast<int>(std::floor(local.y / std::max(m.cell_size.y, 1e-6f))));
    return 2;
}

// entity:tile_to_world(x, y) -> world position of the cell's centre.
int entity_tile_to_world(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto& m = tilemap_of(L, s, e);
    const double x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3);
    const math::Vec3 local{(static_cast<float>(std::floor(x)) + 0.5f) * m.cell_size.x,
                           (static_cast<float>(std::floor(y)) + 0.5f) * m.cell_size.y, 0.0f};
    push_vec3(L, s.world_matrix(e).transform_point(local));
    return 1;
}

// entity:tile_bounds() -> min_x, min_y, max_x, max_y (inclusive) or nil when empty.
int entity_tile_bounds(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const tilemap::TileBounds b = tilemap::bounds(tilemap_of(L, s, e));
    if (b.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, b.min_x);
    lua_pushinteger(L, b.min_y);
    lua_pushinteger(L, b.max_x);
    lua_pushinteger(L, b.max_y);
    return 4;
}

// entity:set_uv_scroll(vector): UV units per second.
int entity_set_uv_scroll(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 v = check_vec3(L, 2);
    overrides_of(s, e).uv_scroll = {v.x, v.y};
    return 0;
}

// entity:set_emissive(vector | nil, strength?): overrides the material's emissive colour (nil
// restores the material's).
int entity_set_emissive(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (lua_isnoneornil(L, 2)) {
        if (auto* o = s.world().get<gameplay::MaterialOverridesComponent>(e)) o->override_emissive = false;
        return 0;
    }
    const math::Vec3 c = check_vec3(L, 2);
    auto& o = overrides_of(s, e);
    o.override_emissive = true;
    o.emissive = {c.x, c.y, c.z, 1};
    if (!lua_isnoneornil(L, 3)) o.emissive_strength = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 3)));
    return 0;
}


// entity:ui_state() -> {visible, hovered, pressed, focused, x, y, width, height, clicks}
// (pixels, from the last layout) or nil before the first layout.
int entity_ui_state(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* st = s.world().get<gameui::UiStateComponent>(e);
    if (!st) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 9);
    const auto set_b = [&](const char* k, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    };
    const auto set_n = [&](const char* k, double v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, k);
    };
    set_b("visible", st->visible);
    set_b("hovered", st->hovered);
    set_b("pressed", st->pressed);
    set_b("focused", st->focused);
    set_n("x", st->rect.x);
    set_n("y", st->rect.y);
    set_n("width", st->rect.w);
    set_n("height", st->rect.h);
    set_n("clicks", static_cast<double>(st->clicks));
    return 1;
}

// ui.screen_size() -> width, height (pixels the UI is laid out for).
int ui_screen_size(lua_State* L) {
    const math::Vec2 sz = gameui::screen_size(scene_of(L));
    lua_pushnumber(L, sz.x);
    lua_pushnumber(L, sz.y);
    return 2;
}

int ui_pointer_over(lua_State* L) {
    lua_pushboolean(L, gameui::pointer_over_ui(scene_of(L)));
    return 1;
}

int ui_focus(lua_State* L) {
    auto& s = scene_of(L);
    gameui::focus(s, lua_isnoneornil(L, 1) ? Entity{} : check_live(L, 1, s));
    return 0;
}

int ui_focused(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = gameui::focused(s);
    push_entity(L, s.world().alive(e) ? e : Entity{});
    return 1;
}

int ui_click(lua_State* L) {
    auto& s = scene_of(L);
    lua_pushboolean(L, gameui::click(s, check_live(L, 1, s)));
    return 1;
}


// loc.text(key, params?) -> string: the key's text in the current locale (then the fallback
// locale) with `{name}` placeholders filled from `params`. Unknown keys return the key itself,
// so a missing translation is visible on screen.
int loc_text(lua_State* L) {
    const std::string_view key = luaL_checkstring(L, 1);
    const std::string* s = host_of(L).loc().find(key);
    const std::string_view pattern = s ? std::string_view(*s) : key;
    if (lua_isnoneornil(L, 2)) {
        lua_pushlstring(L, pattern.data(), pattern.size());
        return 1;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    const std::string out = gameui::format_text(pattern, [L](std::string_view name) -> std::optional<std::string> {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawget(L, 2);
        std::optional<std::string> v;
        if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) {
            size_t len = 0;
            const char* str = luaL_tolstring(L, -1, &len);
            v.emplace(str, len);
            lua_pop(L, 1);
        } else if (lua_isboolean(L, -1)) {
            v.emplace(lua_toboolean(L, -1) ? "true" : "false");
        }
        lua_pop(L, 1);
        return v;
    });
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int loc_has(lua_State* L) {
    lua_pushboolean(L, host_of(L).loc().find(luaL_checkstring(L, 1)) != nullptr);
    return 1;
}

int loc_locale(lua_State* L) {
    const std::string& code = host_of(L).loc().locale();
    lua_pushlstring(L, code.data(), code.size());
    return 1;
}

// loc.set_locale(code) -> bool: switches every keyed UiText and later loc.text calls for the rest
// of the session (scene loads keep it). False when there is no table for `code`.
int loc_set_locale(lua_State* L) {
    lua_pushboolean(L, host_of(L).loc().set_locale(luaL_checkstring(L, 1)));
    return 1;
}

int loc_locales(lua_State* L) {
    const auto codes = host_of(L).loc().locales();
    lua_createtable(L, static_cast<int>(codes.size()), 0);
    for (std::size_t i = 0; i < codes.size(); ++i) {
        lua_pushlstring(L, codes[i].data(), codes[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// loc.name(code?) -> the table's display name ("name" in the file), else the code.
int loc_name(lua_State* L) {
    gameui::Localization& l = host_of(L).loc();
    const std::string code = lua_isnoneornil(L, 1) ? l.locale() : std::string(luaL_checkstring(L, 1));
    const gameui::StringTable* t = l.table(code);
    const std::string& name = t && !t->display_name().empty() ? t->display_name() : code;
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

// entity:play_animation(clip, fade?, restart?) -> bool. Cross-fades the entity's Animator to
// `clip` over `fade` seconds (default: the Animator's crossfade). Before the animation system
// has run on this scene the clip is only recorded on the component (applied next update).
int entity_play_animation(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string clip = luaL_checkstring(L, 2);
    const float fade = static_cast<float>(luaL_optnumber(L, 3, -1.0));
    const bool restart = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4) != 0;
    if (auto* sys = animation::system_of(s)) {
        lua_pushboolean(L, sys->play(s, e, clip, fade, restart));
        return 1;
    }
    auto* a = s.world().get<animation::AnimatorComponent>(e);
    if (a) {
        a->clip = clip;
        a->playing = true;
    }
    lua_pushboolean(L, a != nullptr);
    return 1;
}


// entity:animation() -> {valid, clip, time, duration, playing, looping, finished, blend}
int entity_animation(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    animation::AnimatorStatus st;
    if (auto* sys = animation::system_of(s)) st = sys->status(s, e);
    else if (const auto* a = s.world().get<animation::AnimatorComponent>(e)) {
        st.clip = a->clip;
        st.time = a->time;
        st.playing = a->playing;
        st.looping = a->loop;
    }
    lua_createtable(L, 0, 8);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushstring(L, st.clip.c_str());
    lua_setfield(L, -2, "clip");
    lua_pushnumber(L, st.time);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, st.duration);
    lua_setfield(L, -2, "duration");
    lua_pushboolean(L, st.playing);
    lua_setfield(L, -2, "playing");
    lua_pushboolean(L, st.looping);
    lua_setfield(L, -2, "looping");
    lua_pushboolean(L, st.finished);
    lua_setfield(L, -2, "finished");
    lua_pushnumber(L, st.blend);
    lua_setfield(L, -2, "blend");
    return 1;
}


// entity:animation_clips() -> {names...} (empty until the model resolves)
int entity_animation_clips(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    std::vector<std::string> names;
    if (auto* sys = animation::system_of(s)) names = sys->clips(s, e);
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}


// entity:play_sprite(clip, restart?) -> bool. Plays a clip of the entity's SpriteAnimator sheet.
int entity_play_sprite(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const std::string clip = luaL_checkstring(L, 2);
    const bool restart = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
    if (auto* sys = animation::system_of(s)) {
        lua_pushboolean(L, sys->play_sprite(s, e, clip, restart));
        return 1;
    }
    auto* a = s.world().get<animation::SpriteAnimatorComponent>(e);
    if (a) {
        if (a->clip != clip || restart) a->time = 0;
        a->clip = clip;
        a->playing = true;
    }
    lua_pushboolean(L, a != nullptr);
    return 1;
}


// entity:sprite_animation() -> {valid, clip, frame, clip_frame, time, duration, playing, looping, finished}
int entity_sprite_animation(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    animation::SpriteAnimatorStatus st;
    if (auto* sys = animation::system_of(s)) st = sys->sprite_status(s, e);
    else if (const auto* a = s.world().get<animation::SpriteAnimatorComponent>(e)) {
        st.clip = a->clip;
        st.time = a->time;
        st.playing = a->playing;
    }
    lua_createtable(L, 0, 9);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushstring(L, st.clip.c_str());
    lua_setfield(L, -2, "clip");
    lua_pushnumber(L, st.frame);
    lua_setfield(L, -2, "frame");
    lua_pushnumber(L, st.clip_frame);
    lua_setfield(L, -2, "clip_frame");
    lua_pushnumber(L, st.time);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, st.duration);
    lua_setfield(L, -2, "duration");
    lua_pushboolean(L, st.playing);
    lua_setfield(L, -2, "playing");
    lua_pushboolean(L, st.looping);
    lua_setfield(L, -2, "looping");
    lua_pushboolean(L, st.finished);
    lua_setfield(L, -2, "finished");
    return 1;
}


// entity:sprite_clips() -> {names...} (empty until the sheet resolves)
int entity_sprite_clips(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    std::vector<std::string> names;
    if (auto* sys = animation::system_of(s)) names = sys->sprite_clips(s, e);
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}


// entity:emit_particles(count) -> bool. Queues `count` particles on the entity's
// ParticleEmitter for the next update (works while the emitter is not emitting: bursts on demand).
int entity_emit_particles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const double n = luaL_checknumber(L, 2);
    const auto count = static_cast<std::uint32_t>(std::clamp(n, 0.0, 65536.0));
    lua_pushboolean(L, particles::emit(s, e, count));
    return 1;
}


// entity:set_emitting(on) -> bool. Starts/stops continuous emission (live particles finish).
int entity_set_emitting(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* c = s.world().get<particles::ParticleEmitterComponent>(e);
    if (c) c->emitting = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, c != nullptr);
    return 1;
}


// entity:reset_particles() -> bool. Kills live particles and restarts the emitter cycle.
int entity_reset_particles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    lua_pushboolean(L, particles::reset(s, e));
    return 1;
}


// entity:particles() -> {valid, emitting, alive, emitted, time, finished}
int entity_particles(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const particles::EmitterStatus st = particles::status(s, e);
    const auto* c = s.world().get<particles::ParticleEmitterComponent>(e);
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");
    lua_pushboolean(L, c && c->emitting);
    lua_setfield(L, -2, "emitting");
    lua_pushnumber(L, st.alive);
    lua_setfield(L, -2, "alive");
    lua_pushnumber(L, static_cast<double>(st.emitted));
    lua_setfield(L, -2, "emitted");
    lua_pushnumber(L, st.time);
    lua_setfield(L, -2, "time");
    lua_pushboolean(L, st.finished);
    lua_setfield(L, -2, "finished");
    return 1;
}

} // namespace victus::script::detail
