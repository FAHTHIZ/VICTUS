// SPDX-License-Identifier: Apache-2.0
#include "victus/tilemap/tilemap.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <charconv>
#include <bit>
#include <tuple>
#include <unordered_set>
#include <cmath>
#include <format>
#include <map>
#include <mutex>

namespace victus::tilemap {

using json::Value;
using scene::Entity;
using scene::Scene;

namespace {

Error bad(std::string_view what) { return make_error(ErrorCode::ParseError, std::format("tileset: {}", what)); }

bool read_count(const Value& v, std::uint32_t& out) {
    if (!v.is_number()) return false;
    const double d = v.as_number();
    if (d < 1 || d > kMaxTiles || d != std::floor(d)) return false;
    out = static_cast<std::uint32_t>(d);
    return true;
}

bool read_tile(const Value& v, std::uint32_t tiles, std::int32_t& out, bool allow_empty) {
    if (!v.is_number()) return false;
    const double d = v.as_number();
    if (d != std::floor(d)) return false;
    if (d < 0) {
        if (!allow_empty || d != -1) return false;
        out = -1;
        return true;
    }
    if (d >= tiles) return false;
    out = static_cast<std::int32_t>(d);
    return true;
}

bool in_range(std::int32_t x, std::int32_t y) noexcept {
    return x >= -kMaxCoordinate && x <= kMaxCoordinate && y >= -kMaxCoordinate && y <= kMaxCoordinate;
}

void touch(TilemapComponent& map) { ++map.revision; }

} // namespace

const char* to_string(TileCollision c) noexcept {
    switch (c) {
    case TileCollision::Solid: return "solid";
    case TileCollision::OneWay: return "one_way";
    default: return "none";
    }
}

std::optional<TileCollision> parse_collision(std::string_view s) noexcept {
    if (s == "none") return TileCollision::None;
    if (s == "solid") return TileCollision::Solid;
    if (s == "one_way") return TileCollision::OneWay;
    return std::nullopt;
}

math::Vec4 Tileset::tile_uv(std::uint32_t tile) const noexcept {
    const std::uint32_t cols = std::max(columns, 1u), rws = std::max(rows, 1u);
    tile = std::min(tile, cols * rws - 1);
    const float c = static_cast<float>(tile % cols), r = static_cast<float>(tile / cols);
    const float w = 1.0f / static_cast<float>(cols), h = 1.0f / static_cast<float>(rws);
    return {c * w, r * h, (c + 1) * w, (r + 1) * h};
}

TileCollision Tileset::collision_of(std::int32_t tile) const noexcept {
    if (tile < 0) return TileCollision::None;
    const auto it = collision.find(static_cast<std::uint32_t>(tile));
    return it == collision.end() ? default_collision : it->second;
}

std::int32_t Tileset::terrain_index(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < terrains.size(); ++i)
        if (terrains[i].name == name) return static_cast<std::int32_t>(i);
    return -1;
}

Result<Tileset> parse_tileset(const Value& doc) {
    if (!doc.is_object() || doc["format"].as_string_or("") != "victus.tileset") return bad("not a victus.tileset document");
    const std::int64_t version = doc["version"].as_int(0);
    if (version < 1 || version > kTilesetVersion)
        return make_error(ErrorCode::VersionMismatch, std::format("tileset: unsupported version {}", version));
    Tileset t;
    if (const Value* tex = doc.find("texture"); tex && !tex->is_null()) {
        if (!tex->is_string() || !Uuid::parse(tex->as_string(), t.texture)) return bad("'texture' must be a UUID string");
    }
    if (!read_count(doc["columns"], t.columns)) return bad("'columns' must be a whole number from 1");
    if (!read_count(doc["rows"], t.rows)) return bad("'rows' must be a whole number from 1");
    if (static_cast<std::uint64_t>(t.columns) * t.rows > kMaxTiles)
        return bad(std::format("grid {}x{} has more than {} tiles", t.columns, t.rows, kMaxTiles));
    const std::uint32_t count = t.tile_count();
    if (const Value* c = doc.find("collision"); c && !c->is_null()) {
        const auto parsed = c->is_string() ? parse_collision(c->as_string()) : std::nullopt;
        if (!parsed) return bad("'collision' must be \"none\", \"solid\" or \"one_way\"");
        t.default_collision = *parsed;
    }
    if (const Value* tiles = doc.find("tiles"); tiles && !tiles->is_null()) {
        if (!tiles->is_object()) return bad("'tiles' must be an object keyed by tile index");
        for (const auto& [key, def] : tiles->as_object()) {
            std::uint32_t index = 0;
            const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index >= count)
                return bad(std::format("tile key '{}' is not a tile index in 0..{}", key, count - 1));
            if (!def.is_object()) return bad(std::format("tile {} must be an object", key));
            if (const Value* c = def.find("collision"); c) {
                const auto col = c->is_string() ? parse_collision(c->as_string()) : std::nullopt;
                if (!col) return bad(std::format("tile {}: 'collision' must be \"none\", \"solid\" or \"one_way\"", key));
                t.collision[index] = *col;
            }
        }
    }
    if (const Value* terrains = doc.find("terrains"); terrains && !terrains->is_null()) {
        if (!terrains->is_object()) return bad("'terrains' must be an object of 16-tile lists");
        for (const auto& [name, list] : terrains->as_object()) {
            if (name.empty()) return bad("terrain names must not be empty");
            if (!list.is_array() || list.size() != 16)
                return bad(std::format("terrain '{}' needs exactly 16 tiles (one per neighbour mask)", name));
            Terrain terrain;
            terrain.name = name;
            for (std::size_t i = 0; i < 16; ++i) {
                if (!read_tile(list.as_array()[i], count, terrain.tiles[i], true))
                    return bad(std::format("terrain '{}': entry {} is not a tile in 0..{} (or -1)", name, i, count - 1));
            }
            if (t.terrain_index(name) >= 0) return bad(std::format("terrain '{}' is defined twice", name));
            if (t.terrains.size() >= 32767) return bad("too many terrains");
            t.terrains.push_back(std::move(terrain));
        }
    }
    return t;
}

Value to_json(const Tileset& t) {
    Value v = Value::object();
    v.set("format", "victus.tileset");
    v.set("version", kTilesetVersion);
    v.set("texture", t.texture.is_nil() ? Value{} : Value(t.texture.to_string()));
    v.set("columns", t.columns);
    v.set("rows", t.rows);
    v.set("collision", to_string(t.default_collision));
    Value tiles = Value::object();
    std::vector<std::uint32_t> keys;
    for (const auto& [k, c] : t.collision) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (const std::uint32_t k : keys) {
        Value def = Value::object();
        def.set("collision", to_string(t.collision.at(k)));
        tiles.set(std::to_string(k), std::move(def));
    }
    v.set("tiles", std::move(tiles));
    Value terrains = Value::object();
    for (const auto& terrain : t.terrains) {
        Value list = Value::array();
        for (const std::int32_t tile : terrain.tiles) list.push(tile);
        terrains.set(terrain.name, std::move(list));
    }
    v.set("terrains", std::move(terrains));
    return v;
}

Result<Tileset> load_tileset_file(const std::filesystem::path& path) {
    auto text = fs::read_text(path);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.filename().string(), doc.error().message));
    auto t = parse_tileset(*doc);
    if (!t) return make_error(t.error().code, std::format("{}: {}", path.filename().string(), t.error().message));
    return t;
}

Status save_tileset_file(const std::filesystem::path& path, const Tileset& tileset) {
    return fs::write_text_atomic(path, json::stringify(to_json(tileset), true) + "\n");
}

// ---- component ------------------------------------------------------------------------------

const Cell* TilemapComponent::cell(std::int32_t x, std::int32_t y) const noexcept {
    const auto it = cells.find(cell_key(x, y));
    return it == cells.end() ? nullptr : &it->second;
}

std::int32_t TilemapComponent::tile(std::int32_t x, std::int32_t y) const noexcept {
    const Cell* c = cell(x, y);
    return c ? c->tile : -1;
}

namespace {

// Cells are saved sorted (row by row from the bottom) so scene files diff and hash stably:
// [x, y, tile] or [x, y, tile, flags] (bit 0 flip_x, bit 1 flip_y) or [x, y, tile, flags, terrain].
Value save_cells(const TilemapComponent& m) {
    std::vector<std::pair<std::int64_t, std::int64_t>> order;
    order.reserve(m.cells.size());
    for (const auto& [k, c] : m.cells) order.emplace_back(key_y(k), key_x(k));
    std::sort(order.begin(), order.end());
    Value list = Value::array();
    for (const auto& [y, x] : order) {
        const Cell& c = m.cells.at(cell_key(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)));
        Value e = Value::array();
        e.push(x);
        e.push(y);
        e.push(c.tile);
        const int flags = (c.flip_x ? 1 : 0) | (c.flip_y ? 2 : 0);
        if (flags || c.terrain >= 0) e.push(flags);
        if (c.terrain >= 0) e.push(static_cast<std::int64_t>(c.terrain));
        list.push(std::move(e));
    }
    return list;
}

Status load_cells(TilemapComponent& m, const Value& list) {
    if (!list.is_array()) return make_error(ErrorCode::ParseError, "Tilemap: 'cells' must be an array");
    m.cells.clear();
    for (const Value& e : list.as_array()) {
        const auto& a = e.as_array();
        if (!e.is_array() || a.size() < 3 || a.size() > 5)
            return make_error(ErrorCode::ParseError, "Tilemap: each cell is [x, y, tile, flags?, terrain?]");
        for (const Value& n : a)
            if (!n.is_number() || n.as_number() != std::floor(n.as_number()))
                return make_error(ErrorCode::ParseError, "Tilemap: cell entries must be whole numbers");
        const auto x = static_cast<std::int32_t>(a[0].as_int()), y = static_cast<std::int32_t>(a[1].as_int());
        if (!in_range(x, y) || a[0].as_number() != x || a[1].as_number() != y)
            return make_error(ErrorCode::ParseError, std::format("Tilemap: cell ({}, {}) is out of range", a[0].as_number(), a[1].as_number()));
        Cell c;
        c.tile = static_cast<std::int32_t>(std::clamp<std::int64_t>(a[2].as_int(), -1, kMaxTiles - 1));
        if (a.size() >= 4) {
            const std::int64_t flags = a[3].as_int();
            c.flip_x = (flags & 1) != 0;
            c.flip_y = (flags & 2) != 0;
        }
        if (a.size() >= 5) c.terrain = static_cast<std::int16_t>(std::clamp<std::int64_t>(a[4].as_int(), -1, 32767));
        if (c.tile < 0 && c.terrain < 0) continue;
        m.cells[cell_key(x, y)] = c;
    }
    touch(m);
    return {};
}

} // namespace

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<TilemapComponent> builder("Tilemap", "2D");
        builder.field(&TilemapComponent::tileset, "tileset", {.hint = "tileset"})
                .field(&TilemapComponent::cell_size, "cell_size", {.min = 0.001f, .max = 1e4f, .speed = 0.01f})
                .field(&TilemapComponent::tint, "tint")
                .field(&TilemapComponent::layer, "layer", {.min = -1000, .max = 1000})
                .field(&TilemapComponent::collision, "collision", {.tooltip = "Solid and one-way tiles collide (2D physics)"})
                .field(&TilemapComponent::edge_collision, "edge_collision",
                       {.tooltip = "Merge solid tiles into one-sided edge outlines (smooth sliding across tile seams); "
                                   "off = filled boxes"})
                .field(&TilemapComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
                .field(&TilemapComponent::collision_layer, "collision_layer", {.min = 0, .max = 15})
                .field(&TilemapComponent::collision_mask, "collision_mask", {.hint = "bitmask16"});
        const scene::ComponentSchema* fields = &builder.commit();
        scene::SerializerRegistry::instance().add<TilemapComponent>(
            "Tilemap",
            [fields](const TilemapComponent& m) {
                Value v = fields->save(&m);
                v.set("cells", save_cells(m));
                return v;
            },
            [fields](TilemapComponent& m, const Value& v) -> Status {
                VICTUS_TRY_STATUS(fields->load(&m, v));
                if (const Value* cells = v.find("cells")) return load_cells(m, *cells);
                touch(m);
                return {};
            });
    });
}

// ---- editing --------------------------------------------------------------------------------

bool set_tile(TilemapComponent& map, std::int32_t x, std::int32_t y, std::int32_t tile, bool flip_x, bool flip_y) {
    if (!in_range(x, y)) return false;
    const std::uint64_t k = cell_key(x, y);
    if (tile < 0) {
        if (map.cells.erase(k)) touch(map);
        return true;
    }
    Cell c{tile, -1, flip_x, flip_y};
    auto [it, inserted] = map.cells.try_emplace(k, c);
    if (!inserted) {
        if (it->second == c) return true;
        it->second = c;
    }
    touch(map);
    return true;
}

std::uint64_t fill_tiles(TilemapComponent& map, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                         std::int32_t tile) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (!in_range(x0, y0) || !in_range(x1, y1)) return 0;
    const std::uint64_t n = static_cast<std::uint64_t>(x1 - x0 + 1) * static_cast<std::uint64_t>(y1 - y0 + 1);
    if (n > kMaxFillCells) return 0;
    std::uint64_t changed = 0;
    for (std::int32_t y = y0; y <= y1; ++y)
        for (std::int32_t x = x0; x <= x1; ++x) {
            const std::uint64_t before = map.revision;
            set_tile(map, x, y, tile);
            if (map.revision != before) ++changed;
        }
    return changed;
}

void clear_tiles(TilemapComponent& map) {
    if (map.cells.empty()) return;
    map.cells.clear();
    touch(map);
}

namespace {

std::int32_t pick_terrain_tile(const TilemapComponent& map, const Tileset& ts, std::int32_t x, std::int32_t y,
                               std::int16_t terrain) {
    auto same = [&](std::int32_t cx, std::int32_t cy) {
        const Cell* c = map.cell(cx, cy);
        return c && c->terrain == terrain;
    };
    const int mask = (same(x, y + 1) ? 1 : 0) | (same(x + 1, y) ? 2 : 0) | (same(x, y - 1) ? 4 : 0) | (same(x - 1, y) ? 8 : 0);
    const Terrain& t = ts.terrains[static_cast<std::size_t>(terrain)];
    std::int32_t tile = t.tiles[static_cast<std::size_t>(mask)];
    if (tile < 0) tile = t.tiles[15] >= 0 ? t.tiles[15] : t.tiles[0]; // rules may leave masks unset
    return tile;
}

void retile(TilemapComponent& map, const Tileset& ts, std::int32_t x, std::int32_t y) {
    auto it = map.cells.find(cell_key(x, y));
    if (it == map.cells.end() || it->second.terrain < 0) return;
    if (static_cast<std::size_t>(it->second.terrain) >= ts.terrains.size()) return;
    const std::int32_t tile = pick_terrain_tile(map, ts, x, y, it->second.terrain);
    if (it->second.tile != tile) {
        it->second.tile = tile;
        touch(map);
    }
}

} // namespace

bool paint_terrain(TilemapComponent& map, const Tileset& tileset, std::int32_t x, std::int32_t y, std::int32_t terrain) {
    if (!in_range(x, y)) return false;
    if (terrain >= static_cast<std::int32_t>(tileset.terrains.size())) return false;
    const std::uint64_t k = cell_key(x, y);
    if (terrain < 0) {
        if (map.cells.erase(k)) touch(map);
    } else {
        Cell& c = map.cells[k];
        if (c.terrain != terrain) {
            c = Cell{-1, static_cast<std::int16_t>(terrain), false, false};
            touch(map);
        }
        retile(map, tileset, x, y);
    }
    retile(map, tileset, x, y + 1);
    retile(map, tileset, x + 1, y);
    retile(map, tileset, x, y - 1);
    retile(map, tileset, x - 1, y);
    return true;
}

void refresh_terrain(TilemapComponent& map, const Tileset& tileset) {
    std::vector<std::uint64_t> keys;
    for (const auto& [k, c] : map.cells)
        if (c.terrain >= 0) keys.push_back(k);
    for (const std::uint64_t k : keys) retile(map, tileset, key_x(k), key_y(k));
}

TileBounds bounds(const TilemapComponent& map) noexcept {
    TileBounds b;
    bool first = true;
    for (const auto& [k, c] : map.cells) {
        if (c.tile < 0) continue;
        const std::int32_t x = key_x(k), y = key_y(k);
        if (first) {
            b = {x, y, x, y};
            first = false;
        } else {
            b.min_x = std::min(b.min_x, x);
            b.min_y = std::min(b.min_y, y);
            b.max_x = std::max(b.max_x, x);
            b.max_y = std::max(b.max_y, y);
        }
    }
    return b;
}

std::vector<TileBox> collision_boxes(const TilemapComponent& map, const Tileset& tileset, bool solid) {
    // Row runs: y -> sorted (x, type) of colliding cells.
    std::map<std::int32_t, std::vector<std::pair<std::int32_t, TileCollision>>> rows;
    for (const auto& [k, c] : map.cells) {
        const TileCollision t = tileset.collision_of(c.tile);
        if (t != TileCollision::None && (solid || t != TileCollision::Solid)) rows[key_y(k)].emplace_back(key_x(k), t);
    }
    struct Run {
        std::int32_t x0, x1, y0, y1;
        TileCollision type;
    };
    std::vector<Run> done;
    std::map<std::tuple<std::int32_t, std::int32_t, int>, Run> open; // (x0, x1, type) -> run growing upwards
    for (auto& [y, cells] : rows) {
        std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::map<std::tuple<std::int32_t, std::int32_t, int>, Run> next;
        // One-way tiles never merge vertically: only their top surface is meant to be stood on.
        for (std::size_t i = 0; i < cells.size();) {
            std::size_t j = i + 1;
            while (j < cells.size() && cells[j].first == cells[j - 1].first + 1 && cells[j].second == cells[i].second) ++j;
            const std::int32_t x0 = cells[i].first, x1 = cells[j - 1].first;
            const TileCollision type = cells[i].second;
            const auto key = std::make_tuple(x0, x1, static_cast<int>(type));
            const auto it = open.find(key);
            if (type == TileCollision::Solid && it != open.end() && it->second.y1 == y - 1) {
                Run r = it->second;
                r.y1 = y;
                open.erase(it);
                next[key] = r;
            } else {
                next[key] = Run{x0, x1, y, y, type};
            }
            i = j;
        }
        for (auto& [key, r] : open) done.push_back(r);
        open = std::move(next);
    }
    for (auto& [key, r] : open) done.push_back(r);
    std::vector<TileBox> out;
    out.reserve(done.size());
    const float cw = map.cell_size.x, ch = map.cell_size.y;
    for (const Run& r : done) {
        out.push_back(TileBox{{static_cast<float>(r.x0) * cw, static_cast<float>(r.y0) * ch},
                              {static_cast<float>(r.x1 + 1) * cw, static_cast<float>(r.y1 + 1) * ch},
                              r.type});
    }
    std::sort(out.begin(), out.end(), [](const TileBox& a, const TileBox& b) {
        return a.min.y != b.min.y ? a.min.y < b.min.y : a.min.x < b.min.x;
    });
    return out;
}

std::vector<std::vector<math::Vec2>> collision_outlines(const TilemapComponent& map, const Tileset& tileset) {
    std::unordered_set<std::uint64_t> solid;
    solid.reserve(map.cells.size());
    for (const auto& [k, c] : map.cells)
        if (tileset.collision_of(c.tile) == TileCollision::Solid) solid.insert(k);
    // Directed boundary edges on the cell lattice, solid on the left: vertex -> bit mask of
    // outgoing directions (0 = +x, 1 = +y, 2 = -x, 3 = -y). 64-bit coordinates: cells reach
    // kMaxCoordinate, so their far corners need one more.
    constexpr std::int64_t dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    using V = std::pair<std::int64_t, std::int64_t>;
    std::map<V, std::uint8_t> out;
    const auto is_solid = [&](std::int64_t x, std::int64_t y) {
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return false;
        return solid.contains(cell_key(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)));
    };
    for (const std::uint64_t k : solid) {
        const std::int64_t x = key_x(k), y = key_y(k);
        if (!is_solid(x, y - 1)) out[{x, y}] |= 1u << 0;         // bottom, +x
        if (!is_solid(x + 1, y)) out[{x + 1, y}] |= 1u << 1;     // right, +y
        if (!is_solid(x, y + 1)) out[{x + 1, y + 1}] |= 1u << 2; // top, -x
        if (!is_solid(x - 1, y)) out[{x, y + 1}] |= 1u << 3;     // left, -y
    }
    std::vector<std::vector<math::Vec2>> loops;
    const float cw = map.cell_size.x, ch = map.cell_size.y;
    const auto emit = [&](const std::vector<V>& ring) {
        // Keep only corners (drop points between two collinear edges).
        std::vector<math::Vec2> loop;
        const std::size_t n = ring.size();
        for (std::size_t i = 0; i < n; ++i) {
            const V& a = ring[(i + n - 1) % n];
            const V& b = ring[i];
            const V& c = ring[(i + 1) % n];
            if ((b.first - a.first) * (c.second - b.second) - (b.second - a.second) * (c.first - b.first) == 0) continue;
            loop.push_back({static_cast<float>(b.first) * cw, static_cast<float>(b.second) * ch});
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    };
    std::vector<V> path, stack;
    std::map<V, std::size_t> on_stack;
    while (!out.empty()) {
        // Trace one closed walk. Turning left first keeps solids that touch only at a corner apart.
        auto it = out.begin();
        const V start = it->first;
        int d = std::countr_zero(it->second);
        path.assign(1, start);
        V v = start;
        for (;;) {
            auto at = out.find(v);
            at->second &= static_cast<std::uint8_t>(~(1u << d));
            if (at->second == 0) out.erase(at);
            v = {v.first + dx[d], v.second + dy[d]};
            path.push_back(v);
            at = out.find(v);
            if (at == out.end()) break;
            int next = -1;
            for (const int turn : {1, 0, 3})
                if (at->second & (1u << ((d + turn) & 3))) {
                    next = (d + turn) & 3;
                    break;
                }
            if (next < 0) next = std::countr_zero(at->second); // reversal: unreachable on a lattice boundary
            d = next;
        }
        // The walk ends where it started; split it at repeated vertices into simple loops.
        stack.clear();
        on_stack.clear();
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            const V& p = path[i];
            if (const auto seen = on_stack.find(p); seen != on_stack.end()) {
                std::vector<V> ring(stack.begin() + static_cast<std::ptrdiff_t>(seen->second), stack.end());
                for (std::size_t r = 1; r < ring.size(); ++r) on_stack.erase(ring[r]);
                stack.resize(seen->second + 1);
                emit(ring);
            } else {
                on_stack[p] = stack.size();
                stack.push_back(p);
            }
        }
        if (stack.size() >= 3) emit(stack);
    }
    return loops;
}

// ---- system ---------------------------------------------------------------------------------

TilemapSystem::TilemapSystem(TilesetProvider provider) : provider_(std::move(provider)) { register_components(); }

void TilemapSystem::add_tileset(const Uuid& id, std::shared_ptr<const Tileset> tileset) {
    added_[id] = std::move(tileset);
    ++generation_;
}

void TilemapSystem::clear_cache() {
    cache_.clear();
    ++generation_;
}

std::shared_ptr<const Tileset> TilemapSystem::tileset(const Uuid& id) {
    if (id.is_nil()) return nullptr;
    if (const auto it = added_.find(id); it != added_.end()) return it->second;
    if (const auto it = cache_.find(id); it != cache_.end()) return it->second;
    std::shared_ptr<const Tileset> t = provider_ ? provider_(id) : nullptr;
    cache_[id] = t; // misses are cached too (warned once) until clear_cache()
    return t;
}

void TilemapSystem::update(Scene& scene) {
    stats_.maps = 0;
    stats_.missing_tilesets = 0;
    stats_.boxes = 0;
    stats_.outlines = 0;
    auto& world = scene.world();
    std::vector<Entity> maps;
    world.each<TilemapComponent>([&](Entity e, TilemapComponent&) { maps.push_back(e); });
    for (const Entity e : maps) {
        ++stats_.maps;
        if (!world.get<TilemapRuntime>(e)) world.add<TilemapRuntime>(e);
        // Re-get: adding components may move storage.
        auto* map = world.get<TilemapComponent>(e);
        auto* rt = world.get<TilemapRuntime>(e);
        std::shared_ptr<const Tileset> ts = tileset(map->tileset);
        if (!ts) ++stats_.missing_tilesets;
        rt->tileset = ts;
        rt->resolved = map->tileset;
        const bool want = map->collision && ts;
        const bool edges = map->edge_collision;
        const bool stale = rt->built_revision != map->revision || rt->built_tileset != ts.get() ||
                           rt->built_collision != want || (want && rt->built_edges != edges);
        const auto* shapes = world.get<physics::StaticShapes2DComponent>(e);
        bool settings_changed = false;
        if (want && shapes) {
            settings_changed = shapes->friction != map->friction || shapes->layer != map->collision_layer ||
                               shapes->mask != map->collision_mask;
        }
        if (!stale && !settings_changed && (want == (shapes != nullptr))) {
            stats_.boxes += rt->boxes;
            stats_.outlines += rt->outlines;
            continue;
        }
        if (!want) {
            if (shapes) world.remove<physics::StaticShapes2DComponent>(e);
            rt = world.get<TilemapRuntime>(e);
            map = world.get<TilemapComponent>(e);
            rt->built_revision = map->revision;
            rt->built_tileset = ts.get();
            rt->built_collision = false;
            rt->boxes = 0;
            rt->outlines = 0;
            continue;
        }
        const std::vector<TileBox> boxes = collision_boxes(*map, *ts, !edges);
        physics::StaticShapes2DComponent out;
        if (edges) out.loops = collision_outlines(*map, *ts);
        const auto loops = static_cast<std::uint32_t>(out.loops.size());
        out.boxes.reserve(boxes.size());
        for (const TileBox& b : boxes) out.boxes.push_back({b.min, b.max, b.type == TileCollision::OneWay});
        out.friction = map->friction;
        out.layer = map->collision_layer;
        out.mask = map->collision_mask;
        out.revision = (shapes ? shapes->revision : 0) + 1;
        const std::uint64_t revision = map->revision;
        const void* built = ts.get();
        const auto count = static_cast<std::uint32_t>(boxes.size());
        if (auto* existing = world.get<physics::StaticShapes2DComponent>(e)) *existing = std::move(out);
        else world.add<physics::StaticShapes2DComponent>(e, std::move(out));
        rt = world.get<TilemapRuntime>(e);
        rt->built_revision = revision;
        rt->built_tileset = built;
        rt->built_collision = true;
        rt->built_edges = edges;
        rt->boxes = count;
        rt->outlines = loops;
        stats_.boxes += count;
        stats_.outlines += loops;
        ++stats_.collision_rebuilds;
    }
}

void add_tilemap_systems(gameplay::Simulation& sim, TilemapSystem& system) {
    TilemapSystem* sys = &system;
    sim.add_system("tilemap.pre", gameplay::Phase::PreUpdate, [sys](Scene& scene, const gameplay::FrameTime&) { sys->update(scene); });
    sim.add_system("tilemap.post", gameplay::Phase::PostUpdate, [sys](Scene& scene, const gameplay::FrameTime&) { sys->update(scene); });
}

} // namespace victus::tilemap
