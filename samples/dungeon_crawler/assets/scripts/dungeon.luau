-- Dungeon Crawler game rules: builds a random maze for each floor at run time with the tilemap
-- API (fill_tiles for the floor, paint_terrain for autotiled walls), scatters gems and slimes,
-- and opens the exit portal once every gem on the floor is collected. Lives on the "Dungeon"
-- entity, next to the "Floor" and "Walls" tilemaps. The hero walks with its own script.
local Dungeon = {}
Dungeon.cells_x = 8        -- maze size in rooms (tiles = 2 * cells + 1)
Dungeon.cells_y = 6
Dungeon.growth = 2         -- extra rooms per floor on each axis
Dungeon.floors = 3         -- floors to clear to win
Dungeon.gems = 6           -- gems on the first floor (+2 per floor)
Dungeon.slimes = 2         -- slimes on the first floor (+1 per floor)
Dungeon.slime_speed = 2.2  -- tiles per second
Dungeon.loops = 0.12       -- share of inner walls knocked out so the maze has loops
Dungeon.lives = 3
Dungeon.texture = "textures/dungeon_tiles.png"
Dungeon.gem_sound = "audio/coin.wav"
Dungeon.exit_sound = "audio/jump.wav"

local FLOOR_TILES = { 16, 16, 16, 17, 18 }
local DECOR_TILES = { 21, 22, 23 }
local EXIT_CLOSED, EXIT_OPEN = 19, 20
local HERO, GEM, SLIME = 24, 25, 26
local COLUMNS, ROWS = 4, 7

-- UV rectangle (u0, v0 top, u1, v1 bottom) of a tile of the shared texture.
local function tile_uv(index)
    local c, r = index % COLUMNS, index // COLUMNS
    return { c / COLUMNS, r / ROWS, (c + 1) / COLUMNS, (r + 1) / ROWS }
end
Dungeon.tile_uv = tile_uv

local function key(x, y) return x * 4096 + y end

-- Sprite of a tile of the shared texture (the Sprite loader replaces every field, so the whole
-- component is written, then the texture is set by path).
function Dungeon:set_sprite(e, index, size, layer, flip_x)
    e:set("Sprite", { size = { size, size }, layer = layer, uv = tile_uv(index), flip_x = flip_x or false })
    e:set_sprite(self.texture)
end

function Dungeon:start()
    self.floor_map = scene.find("Floor")
    self.walls = scene.find("Walls")
    self.hero = scene.find("Hero")
    assert(self.floor_map and self.walls and self.hero, "Dungeon needs Floor, Walls and Hero entities")
    self:set_sprite(self.hero, HERO, 0.9, 4)
    self.level = 0
    self.lives_left = self.lives
    self.won = false
    self.actors = {}
    self:next_floor()
end

-- ---- maze ---------------------------------------------------------------------------------

-- Recursive backtracker over rooms at odd tile coordinates, plus a few knocked-out walls.
function Dungeon:carve(cx, cy)
    local w, h = 2 * cx + 1, 2 * cy + 1
    local open = {}
    local stack = { { 0, 0 } }
    local seen = { [key(0, 0)] = true }
    open[key(1, 1)] = true
    local dirs = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }
    while #stack > 0 do
        local top = stack[#stack]
        local choices = {}
        for _, d in dirs do
            local nx, ny = top[1] + d[1], top[2] + d[2]
            if nx >= 0 and ny >= 0 and nx < cx and ny < cy and not seen[key(nx, ny)] then
                table.insert(choices, { nx, ny, d })
            end
        end
        if #choices == 0 then
            table.remove(stack)
        else
            local pick = choices[math.random(#choices)]
            seen[key(pick[1], pick[2])] = true
            open[key(2 * top[1] + 1 + pick[3][1], 2 * top[2] + 1 + pick[3][2])] = true
            open[key(2 * pick[1] + 1, 2 * pick[2] + 1)] = true
            table.insert(stack, { pick[1], pick[2] })
        end
    end
    for x = 1, w - 2 do
        for y = 1, h - 2 do
            -- A wall between two rooms (odd/even pattern) may become a passage.
            if not open[key(x, y)] and (x + y) % 2 == 1 and math.random() < self.loops then open[key(x, y)] = true end
        end
    end
    return open, w, h
end

function Dungeon:clear_actors()
    for _, a in self.actors do
        if a.entity.valid then a.entity:destroy() end
    end
    self.actors = {}
end

function Dungeon:spawn_actor(kind, name, x, y, size, layer)
    local e = scene.spawn(name)
    self:set_sprite(e, kind, size, layer)
    local p = self.walls:tile_to_world(x, y)
    e.position = vector.create(p.x, p.y, 0)
    local actor = { entity = e, kind = kind, x = x, y = y, tx = x, ty = y, phase = math.random() * 6.28,
        size = size, layer = layer, flip = false }
    table.insert(self.actors, actor)
    return actor
end

-- Random open cells far enough from the start.
function Dungeon:free_cells(min_distance)
    local cells = {}
    for k in self.open do
        local x, y = k // 4096, k % 4096
        if math.abs(x - 1) + math.abs(y - 1) >= min_distance and not (x == self.exit_x and y == self.exit_y) then
            table.insert(cells, { x, y })
        end
    end
    table.sort(cells, function(a, b) return key(a[1], a[2]) < key(b[1], b[2]) end) -- deterministic order
    for i = #cells, 2, -1 do
        local j = math.random(i)
        cells[i], cells[j] = cells[j], cells[i]
    end
    return cells
end

function Dungeon:next_floor()
    self.level += 1
    self:clear_actors()
    local cx = self.cells_x + (self.level - 1) * self.growth
    local cy = self.cells_y + (self.level - 1) * self.growth
    local open, w, h = self:carve(cx, cy)
    self.open, self.width, self.height = open, w, h
    -- The exit is the room farthest (in steps) from the start.
    local dist, queue, head = { [key(1, 1)] = 0 }, { { 1, 1 } }, 1
    local far = { 1, 1 }
    while head <= #queue do
        local c = queue[head]
        head += 1
        for _, d in { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } do
            local nx, ny = c[1] + d[1], c[2] + d[2]
            if open[key(nx, ny)] and dist[key(nx, ny)] == nil then
                dist[key(nx, ny)] = dist[key(c[1], c[2])] + 1
                table.insert(queue, { nx, ny })
                if dist[key(nx, ny)] > dist[key(far[1], far[2])] then far = { nx, ny } end
            end
        end
    end
    self.exit_x, self.exit_y = far[1], far[2]

    self.walls:clear_tiles()
    self.floor_map:clear_tiles()
    self.floor_map:fill_tiles(0, 0, w - 1, h - 1, 16)
    for x = 0, w - 1 do
        for y = 0, h - 1 do
            if open[key(x, y)] then
                local t = math.random() < 0.06 and DECOR_TILES[math.random(#DECOR_TILES)]
                    or FLOOR_TILES[math.random(#FLOOR_TILES)]
                self.floor_map:set_tile(x, y, t)
            else
                self.walls:paint_terrain(x, y, "wall")
            end
        end
    end
    self.floor_map:set_tile(self.exit_x, self.exit_y, EXIT_CLOSED)
    self.exit_open = false

    local cells = self:free_cells(4)
    local gems = math.min(self.gems + 2 * (self.level - 1), #cells)
    for i = 1, gems do self:spawn_actor(GEM, "Gem", cells[i][1], cells[i][2], 0.6, 2) end
    self.gems_left = gems
    local slimes = math.min(self.slimes + (self.level - 1), #cells - gems)
    for i = 1, slimes do
        local c = cells[gems + i]
        self:spawn_actor(SLIME, "Slime", c[1], c[2], 0.8, 3)
    end
    self:respawn_hero()
    log.info(string.format("Floor %d: %dx%d tiles, %d gems, %d slimes", self.level, w, h, gems, slimes))
    self:refresh_hud()
end

function Dungeon:respawn_hero()
    local p = self.walls:tile_to_world(1, 1)
    self.hero.position = vector.create(p.x, p.y, 0)
    self.invulnerable = 1.5
end

-- ---- HUD ----------------------------------------------------------------------------------

function Dungeon:refresh_hud()
    local status = scene.find("Status Text")
    if status then
        status:set_text(string.format("Floor %d / %d    Gems left %d    Lives %d", self.level, self.floors,
            self.gems_left, self.lives_left))
    end
end

function Dungeon:banner(text)
    local b = scene.find("Banner")
    if b then
        b:set_text(text or "")
        b:set_visible(text ~= nil)
    end
end

-- ---- play ---------------------------------------------------------------------------------

-- Slimes walk from room to room, preferring passages towards the hero when it is near.
function Dungeon:move_slime(a, dt, hero_x, hero_y)
    local target = self.walls:tile_to_world(a.tx, a.ty)
    local p = a.entity.position
    local to = vector.create(target.x - p.x, target.y - p.y, 0)
    local len = vector.magnitude(to)
    local step = self.slime_speed * dt
    if len > step then
        a.entity.position = p + to * (step / len)
        return
    end
    a.entity.position = vector.create(target.x, target.y, 0)
    a.x, a.y = a.tx, a.ty
    local options = {}
    for _, d in { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } do
        local nx, ny = a.x + d[1], a.y + d[2]
        if self.open[key(nx, ny)] and not (nx == a.px and ny == a.py) then table.insert(options, { nx, ny }) end
    end
    if #options == 0 then options = { { a.px or a.x, a.py or a.y } } end -- dead end: turn back
    local pick = options[math.random(#options)]
    local near = math.abs(hero_x - a.x) + math.abs(hero_y - a.y) <= 6
    if near and math.random() < 0.7 then
        local best = math.huge
        for _, o in options do
            local d = math.abs(hero_x - o[1]) + math.abs(hero_y - o[2])
            if d < best then best, pick = d, o end
        end
    end
    a.px, a.py = a.x, a.y
    a.tx, a.ty = pick[1], pick[2]
end

function Dungeon:update(dt)
    if self.won then
        if input.action_pressed("restart") then
            self.level, self.lives_left, self.won = 0, self.lives, false
            self:banner(nil)
            self:next_floor()
        end
        return
    end
    if input.action_pressed("restart") then
        self.level -= 1
        self:next_floor()
        return
    end
    self.invulnerable = math.max(0, (self.invulnerable or 0) - dt)
    local hp = self.hero.position
    local hx, hy = self.walls:world_to_tile(hp)
    for i = #self.actors, 1, -1 do
        local a = self.actors[i]
        a.phase += dt * 4
        if a.kind == GEM then
            local base = self.walls:tile_to_world(a.x, a.y)
            a.entity.position = vector.create(base.x, base.y + math.sin(a.phase) * 0.08, 0)
            if vector.magnitude(a.entity.position - hp) < 0.6 then
                a.entity:destroy()
                table.remove(self.actors, i)
                self.gems_left -= 1
                audio.play(self.gem_sound)
                if self.gems_left == 0 then
                    self.exit_open = true
                    self.floor_map:set_tile(self.exit_x, self.exit_y, EXIT_OPEN)
                    log.info("The exit is open")
                end
                self:refresh_hud()
            end
        elseif a.kind == SLIME then
            self:move_slime(a, dt, hx, hy)
            local flip = a.tx < a.x or (a.tx == a.x and a.flip)
            if flip ~= a.flip then
                a.flip = flip
                self:set_sprite(a.entity, SLIME, a.size, a.layer, flip)
            end
            if self.invulnerable <= 0 and vector.magnitude(a.entity.position - hp) < 0.65 then
                self.lives_left -= 1
                log.info("Hit by a slime, lives left:", self.lives_left)
                if self.lives_left <= 0 then
                    self.lives_left = self.lives
                    self.level = 0
                    self:next_floor()
                    self:banner("Game over - back to floor 1")
                    self.banner_time = 3
                    return
                end
                self:respawn_hero()
                self:refresh_hud()
            end
        end
    end
    if self.banner_time then
        self.banner_time -= dt
        if self.banner_time <= 0 then
            self.banner_time = nil
            self:banner(nil)
        end
    end
    if self.exit_open and hx == self.exit_x and hy == self.exit_y then
        audio.play(self.exit_sound)
        if self.level >= self.floors then
            self.won = true
            self:clear_actors()
            log.info("Dungeon cleared!")
            self:banner("Dungeon cleared! Press R to play again")
        else
            self:next_floor()
            self:banner(string.format("Floor %d", self.level))
            self.banner_time = 1.5
        end
    end
end

return Dungeon
