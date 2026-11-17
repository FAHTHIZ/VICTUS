-- Robot Arena game rules (on the "Arena" entity). Spawns waves of hostile robots from the four
-- gates, drives them over the navmesh towards the player, resolves the player's swings queued
-- in the shared arena state, drops health pickups, keeps score and runs the HUD. Clearing
-- every wave wins; losing all health ends the run. "restart" (R, gamepad Start) plays again.
-- Checkpoints (F5/F9 through the HUD script) carry the wave, score, health and every robot.
local State = require("scripts/arena_state.luau")

local Arena = {}
Arena.waves = 5
Arena.first_wave = 3          -- robots in wave 1 (+1 per wave)
Arena.enemy = "prefabs/enemy_robot.vprefab"
Arena.pickup = "prefabs/health_orb.vprefab"
Arena.enemy_health = 3        -- swings to destroy a robot
Arena.enemy_damage = 7        -- health taken per robot hit
Arena.enemy_reach = 1.6
Arena.enemy_cooldown = 1.2    -- seconds between a robot's hits
Arena.player_health = 100
Arena.pickup_heal = 30
Arena.pickup_chance = 0.3     -- chance that a destroyed robot drops a pickup
Arena.spawn_interval = 0.7    -- seconds between robots of a wave
Arena.wave_pause = 2.5        -- seconds between waves
Arena.hit_sound = "audio/impact.wav"
Arena.gates = { vector.create(-12, 0, -12), vector.create(12, 0, -12), vector.create(12, 0, 12), vector.create(-12, 0, 12) }

local function flat(v) return vector.create(v.x, 0, v.z) end

function Arena:start()
    State.reset(self.player_health)
    self.player = scene.find("Player")
    self.burst = scene.find("Hit Burst")
    assert(self.player, "Arena needs a Player entity")
    self.robots = {}  -- { entity, health, cooldown, flash, goal, clip }
    self.orbs = {}    -- { entity, phase }
    self.wave = 0
    self.to_spawn = 0
    self.spawn_timer = 0
    self.pause = 1.0
    self.score = 0
    self.kills = 0
    self.won = false
    self.lost = false
    self:banner("Robot Arena")
    self.banner_time = 1.5
    self:refresh_hud()
end

-- ---- checkpoints --------------------------------------------------------------------------

function Arena:save_state()
    local robots = {}
    for _, r in self.robots do table.insert(robots, { entity = r.entity, health = r.health }) end
    local orbs = {}
    for _, o in self.orbs do table.insert(orbs, o.entity) end
    return { wave = self.wave, to_spawn = self.to_spawn, score = self.score, kills = self.kills,
             health = State.health, robots = robots, orbs = orbs }
end

function Arena:load_state(state)
    self.wave, self.to_spawn = state.wave or 0, state.to_spawn or 0
    self.score, self.kills = state.score or 0, state.kills or 0
    State.health = state.health or self.player_health
    self.pause = 0
    for _, r in state.robots or {} do
        if r.entity then self:track_robot(r.entity, r.health) end
    end
    for _, o in state.orbs or {} do
        if o then table.insert(self.orbs, { entity = o, phase = 0 }) end
    end
    self:banner(nil)
    self:refresh_hud()
end

-- ---- HUD ----------------------------------------------------------------------------------

function Arena:refresh_hud()
    local status = scene.find("Status Text")
    if status then
        status:set_text(string.format("Wave %d / %d    Robots %d    Health %d    Score %d", self.wave, self.waves,
            #self.robots + self.to_spawn, math.max(0, math.ceil(State.health)), self.score))
    end
end

function Arena:banner(text)
    local b = scene.find("Banner")
    if b then
        b:set_text(text or "")
        b:set_visible(text ~= nil)
    end
    self.banner_time = nil
end

-- ---- robots -------------------------------------------------------------------------------

function Arena:track_robot(e, health)
    local r = { entity = e, health = health or self.enemy_health, cooldown = 0.5, flash = 0, clip = "Idle" }
    r.robot = e:find_child("Robot")
    r.body = r.robot and r.robot:find_child("Body")
    table.insert(self.robots, r)
    State.enemies = self:robot_entities()
end

function Arena:robot_entities()
    local list = {}
    for _, r in self.robots do table.insert(list, r.entity) end
    return list
end

function Arena:spawn_robot()
    local gate = self.gates[math.random(#self.gates)]
    local jitter = vector.create(math.random() * 2 - 1, 0.1, math.random() * 2 - 1)
    local e = scene.instantiate(self.enemy, { position = gate + jitter, name = "Enemy Robot" })
    if e then self:track_robot(e) end
end

function Arena:set_flash(r, on)
    if not r.body then return end
    for _, part in r.body:children() do
        part:set_emissive(if on then vector.create(1, 0.9, 0.6) else nil, 3)
    end
end

function Arena:destroy_robot(i)
    local r = self.robots[i]
    local p = r.entity.position
    if self.burst then
        self.burst.position = p + vector.create(0, 0.8, 0)
        self.burst:emit_particles(40)
    end
    audio.play(self.hit_sound, { position = p, pitch = 0.7 })
    if math.random() < self.pickup_chance or (State.health < 35 and #self.orbs == 0) then
        local orb = scene.instantiate(self.pickup, { position = vector.create(p.x, 0.6, p.z), name = "Health Orb" })
        if orb then table.insert(self.orbs, { entity = orb, phase = 0 }) end
    end
    scene.destroy(r.entity)
    table.remove(self.robots, i)
    self.kills += 1
    self.score += 100 * self.wave
    State.enemies = self:robot_entities()
end

-- Applies the player's queued swings to every robot in reach and in front of it.
function Arena:resolve_attacks()
    for _, a in State.attacks do
        local hit = false
        for i = #self.robots, 1, -1 do
            local r = self.robots[i]
            local to = flat(r.entity.position - a.origin)
            local d = vector.magnitude(to)
            if d <= a.range and (d < 0.6 or vector.dot(to / d, a.facing) >= a.arc) then
                hit = true
                r.health -= 1
                r.flash = 0.12
                self:set_flash(r, true)
                if r.health <= 0 then self:destroy_robot(i) end
            end
        end
        if hit then audio.play(self.hit_sound, { position = a.origin, pitch = 1.2 + math.random() * 0.2 }) end
    end
    State.attacks = {}
end

function Arena:update_robots(dt)
    local target = self.player.position
    for _, r in self.robots do
        local e = r.entity
        local p = e.position
        local to = flat(target - p)
        local d = vector.magnitude(to)
        r.cooldown = math.max(0, r.cooldown - dt)
        if d <= self.enemy_reach then
            if r.goal then
                e:stop()
                r.goal = nil
            end
            if r.robot and d > 0.01 then r.robot.rotation = vector.create(0, math.deg(math.atan2(to.x, to.z)), 0) end
            if r.cooldown <= 0 and not self.lost then
                r.cooldown = self.enemy_cooldown
                State.health -= self.enemy_damage
                State.hurt = 0.2
                audio.play(self.hit_sound, { position = p, pitch = 0.9 })
            end
        elseif not r.goal or vector.magnitude(flat(target - r.goal)) > 0.8 then
            if e:set_destination(target) then r.goal = target end
        end
        if r.robot then
            -- The robot model faces its travel direction (the NavAgent turns the root otherwise).
            local v = flat(e.velocity)
            if vector.magnitude(v) > 0.3 then r.robot.rotation = vector.create(0, 0, 0) end
            local clip = if vector.magnitude(v) > 0.3 then "Walk" else "Idle"
            if clip ~= r.clip then
                r.clip = clip
                r.robot:play_animation(clip, 0.2)
            end
        end
        if r.flash > 0 then
            r.flash -= dt
            if r.flash <= 0 then self:set_flash(r, false) end
        end
    end
end

function Arena:update_orbs(dt)
    local p = self.player.position
    for i = #self.orbs, 1, -1 do
        local o = self.orbs[i]
        if not o.entity or not o.entity.valid then
            table.remove(self.orbs, i)
        elseif vector.magnitude(flat(o.entity.position - p)) < 1.2 then
            State.health = math.min(State.max_health, State.health + self.pickup_heal)
            audio.play(self.hit_sound, { position = p, pitch = 1.8 })
            scene.destroy(o.entity)
            table.remove(self.orbs, i)
        end
    end
    local list = {}
    for _, o in self.orbs do table.insert(list, o.entity) end
    State.pickups = list
end

-- ---- flow ---------------------------------------------------------------------------------

function Arena:update(dt)
    if self.banner_time then
        self.banner_time -= dt
        if self.banner_time <= 0 then self:banner(nil) end
    end
    if self.won or self.lost then
        State.over = true
        if input.action_pressed("restart") then scene.reload() end
        return
    end
    State.hurt = math.max(0, State.hurt - dt)
    self:resolve_attacks()
    self:update_robots(dt)
    self:update_orbs(dt)

    if State.health <= 0 then
        self.lost = true
        State.over = true
        for _, r in self.robots do r.entity:stop() end
        log.info(string.format("Defeated on wave %d with %d points", self.wave, self.score))
        self:banner("Defeated - press R to try again")
    elseif self.to_spawn > 0 then
        self.spawn_timer -= dt
        if self.spawn_timer <= 0 then
            self.spawn_timer = self.spawn_interval
            self.to_spawn -= 1
            self:spawn_robot()
        end
    elseif #self.robots == 0 then
        if self.wave >= self.waves then
            self.won = true
            State.over = true
            log.info(string.format("Arena cleared! %d robots, %d points, %d health left", self.kills, self.score,
                math.ceil(State.health)))
            self:banner("Arena cleared! Press R to play again")
        else
            self.pause -= dt
            if self.pause <= 0 then
                self.wave += 1
                self.to_spawn = self.first_wave + self.wave - 1
                self.spawn_timer = 0
                self.pause = self.wave_pause
                log.info(string.format("Wave %d: %d robots", self.wave, self.to_spawn))
                self:banner(string.format("Wave %d", self.wave))
                self.banner_time = 1.5
            end
        end
    end
    self:refresh_hud()
end

return Arena
