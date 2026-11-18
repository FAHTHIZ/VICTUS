-- Ruin Runner game rules: builds each level from the ASCII maps in scripts/levels.luau with the
-- tilemap API ("Ground" collides, "Decor" does not), spawns coins and beetles, and handles
-- spikes, falls, stomps, checkpoints, lives and the goal banner. Lives on the "Game" entity; the
-- hero runs and jumps with its own script (scripts/hero.luau).
local Levels = require("scripts/levels")

local Game = {}
Game.lives = 5
Game.start_level = 1
Game.beetle_speed = 1.6 -- tiles per second
Game.stomp_bounce = 9
Game.texture = "textures/ruin_tiles.png"
Game.coin_sound = "audio/coin.wav"
Game.jump_sound = "audio/jump.wav"

local GRASS, EARTH, PLANK = 0, 1, 3
local SPIKES, FLAG_DOWN, FLAG_UP, GOAL, BUSH = 4, 5, 6, 7, 14
local COIN, BEETLE_A, BEETLE_B = 11, 12, 13
local COLUMNS, ROWS = 4, 4
Game.tiles = { spikes = SPIKES, plank = PLANK, goal = GOAL }

local function tile_uv(index)
    local c, r = index % COLUMNS, index // COLUMNS
    return { c / COLUMNS, r / ROWS, (c + 1) / COLUMNS, (r + 1) / ROWS }
end
Game.tile_uv = tile_uv

-- Writes the whole Sprite component (the loader replaces every field), then the texture.
function Game:set_sprite(e, index, size, layer, flip_x)
    e:set("Sprite", { size = { size, size }, layer = layer, uv = tile_uv(index), flip_x = flip_x or false })
    e:set_sprite(self.texture)
end

function Game:start()
    self.ground = scene.find("Ground")
    self.decor = scene.find("Decor")
    self.hero = scene.find("Hero")
    assert(self.ground and self.decor and self.hero, "Game needs Ground, Decor and Hero entities")
    self.actors = {}
    self.won = false
    self.lives_left = self.lives
    self.coins = 0
    self:load_level(math.clamp(self.start_level, 1, #Levels))
end

-- ---- level building -----------------------------------------------------------------------

function Game:clear_actors()
    for _, a in self.actors do
        if a.entity.valid then a.entity:destroy() end
    end
    self.actors = {}
end

function Game:spawn(kind, name, x, y, size, layer)
    local e = scene.spawn(name)
    self:set_sprite(e, kind, size, layer)
    local p = self.ground:tile_to_world(x, y)
    e.position = vector.create(p.x, p.y, 0)
    local a = { entity = e, kind = kind, x = x, y = y, dir = -1, phase = 0, size = size, layer = layer }
    table.insert(self.actors, a)
    return a
end

function Game:load_level(n)
    self.level = n
    self:clear_actors()
    self.ground:clear_tiles()
    self.decor:clear_tiles()
    self.checkpoints = {}
    local rows = Levels[n]
    local h = #rows
    self.width, self.height = 0, h
    local function at(col, row) return string.sub(rows[row], col, col) end
    for row = 1, h do
        local line = rows[row]
        self.width = math.max(self.width, #line)
        local y = h - row
        for col = 1, #line do
            local x, c = col - 1, string.sub(line, col, col)
            if c == "#" then
                local above = row > 1 and at(col, row - 1) or " "
                self.ground:set_tile(x, y, above == "#" and EARTH or GRASS)
            elseif c == "=" then
                self.ground:set_tile(x, y, PLANK)
            elseif c == "^" then
                self.decor:set_tile(x, y, SPIKES)
            elseif c == "*" then
                self.decor:set_tile(x, y, BUSH)
            elseif c == "C" then
                self.decor:set_tile(x, y, FLAG_DOWN)
                table.insert(self.checkpoints, { x = x, y = y, on = false })
            elseif c == "G" then
                self.decor:set_tile(x, y, GOAL)
                self.goal_x, self.goal_y = x, y
            elseif c == "o" then
                self:spawn(COIN, "Coin", x, y, 0.6, 2)
            elseif c == "E" then
                self:spawn(BEETLE_A, "Beetle", x, y, 0.9, 3)
            elseif c == "P" then
                self.start_x, self.start_y = x, y
            end
        end
    end
    self.spawn_x, self.spawn_y = self.start_x, self.start_y
    self:respawn_hero()
    local camera = scene.find("Main Camera")
    if camera then
        local p = self.hero.position
        camera.position = vector.create(p.x, p.y + 1.5, camera.position.z)
    end
    log.info(string.format("Level %d: %d x %d tiles", n, self.width, h))
    self:refresh_hud()
end

function Game:respawn_hero()
    local p = self.ground:tile_to_world(self.spawn_x, self.spawn_y)
    self.hero.position = vector.create(p.x, p.y, 0)
    self.hero.velocity = vector.create(0, 0, 0)
    self.invulnerable = 1.0
end

-- ---- HUD ----------------------------------------------------------------------------------

function Game:refresh_hud()
    local status = scene.find("Status Text")
    if status then
        status:set_text(string.format("Level %d / %d    Coins %d    Lives %d", self.level, #Levels, self.coins,
            self.lives_left))
    end
end

function Game:banner(text, seconds)
    local b = scene.find("Banner")
    if b then
        b:set_text(text or "")
        b:set_visible(text ~= nil)
    end
    self.banner_time = seconds
end

-- ---- play ---------------------------------------------------------------------------------

function Game:solid(x, y)
    local t = self.ground:tile(x, y)
    return t ~= nil and t ~= PLANK
end

function Game:standable(x, y)
    return self.ground:tile(x, y) ~= nil
end

-- Beetles walk along their ledge and turn at walls, edges and spikes.
function Game:move_beetle(a, dt)
    local p = a.entity.position
    local nx = p.x + a.dir * self.beetle_speed * dt
    local ahead = self.ground:world_to_tile(vector.create(nx + a.dir * 0.45, p.y, 0))
    if self:solid(ahead, a.y) or not self:standable(ahead, a.y - 1) or self.decor:tile(ahead, a.y) == SPIKES then
        a.dir = -a.dir
        return
    end
    a.entity.position = vector.create(nx, p.y, 0)
    a.phase += dt * 6
    local frame = (math.floor(a.phase) % 2 == 0) and BEETLE_A or BEETLE_B
    local flip = a.dir > 0
    if frame ~= a.frame or flip ~= a.flip then
        a.frame, a.flip = frame, flip
        self:set_sprite(a.entity, frame, a.size, a.layer, flip)
    end
end

function Game:lose_life(reason)
    self.lives_left -= 1
    log.info(string.format("Lost a life (%s), lives left: %d", reason, self.lives_left))
    if self.lives_left <= 0 then
        self.lives_left = self.lives
        self.coins = 0
        self:load_level(self.level)
        self:banner("Game over - the level restarts", 2.5)
        return
    end
    self:respawn_hero()
    self:refresh_hud()
end

function Game:update(dt)
    if self.won then
        if input.action_pressed("restart") then
            self.won, self.coins, self.lives_left = false, 0, self.lives
            self:banner(nil)
            self:load_level(1)
        end
        return
    end
    if input.action_pressed("restart") then
        self:load_level(self.level)
        return
    end
    if self.banner_time then
        self.banner_time -= dt
        if self.banner_time <= 0 then self:banner(nil) end
    end
    self.invulnerable = math.max(0, (self.invulnerable or 0) - dt)
    local hp = self.hero.position
    local feet = vector.create(hp.x, hp.y - 0.3, 0)
    local fx, fy = self.ground:world_to_tile(feet)
    if hp.y < -3 then return self:lose_life("fell") end
    if self.invulnerable <= 0 and self.decor:tile(fx, fy) == SPIKES then return self:lose_life("spikes") end
    for i = #self.actors, 1, -1 do
        local a = self.actors[i]
        local e = a.entity
        if a.kind == COIN then
            local base = self.ground:tile_to_world(a.x, a.y)
            a.phase += dt * 3
            e.position = vector.create(base.x, base.y + math.sin(a.phase) * 0.1, 0)
            if vector.magnitude(e.position - hp) < 0.65 then
                e:destroy()
                table.remove(self.actors, i)
                self.coins += 1
                audio.play(self.coin_sound)
                self:refresh_hud()
            end
        else
            self:move_beetle(a, dt)
            local d = e.position - hp
            if math.abs(d.x) < 0.75 and math.abs(d.y) < 0.8 then
                if hp.y - e.position.y > 0.3 and self.hero.velocity.y <= 0.5 then
                    e:destroy()
                    table.remove(self.actors, i)
                    self.hero.velocity = vector.create(self.hero.velocity.x, self.stomp_bounce, 0)
                    audio.play(self.jump_sound, { volume = 0.5 })
                    log.info("Stomped a beetle")
                elseif self.invulnerable <= 0 then
                    return self:lose_life("beetle")
                end
            end
        end
    end
    for _, c in self.checkpoints do
        if not c.on and fx >= c.x and math.abs(fy - c.y) <= 2 then
            c.on = true
            self.spawn_x, self.spawn_y = c.x, c.y
            self.decor:set_tile(c.x, c.y, FLAG_UP)
            audio.play(self.coin_sound, { pitch = 1.5 })
            log.info(string.format("Checkpoint at %d,%d", c.x, c.y))
        end
    end
    if self.goal_x and math.abs(fx - self.goal_x) <= 0 and math.abs(fy - self.goal_y) <= 1 then
        audio.play(self.jump_sound)
        if self.level >= #Levels then
            self.won = true
            self:clear_actors()
            log.info(string.format("Ruins cleared! %d coins", self.coins))
            self:banner("Ruins cleared! Press R to play again")
        else
            self:load_level(self.level + 1)
            self:banner(string.format("Level %d", self.level), 1.5)
        end
    end
end

return Game
