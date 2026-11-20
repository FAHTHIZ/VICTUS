-- The arena fighter. Walks relative to the camera with "move_x"/"move_y" (WASD, arrows, left
-- stick), sprints with "sprint", jumps with "jump" and swings with "attack" (left mouse, J,
-- gamepad X / right trigger); swings are queued in the shared arena state and resolved by the
-- arena rules against every enemy in front of the player. The child "Robot" model faces the
-- direction of travel and cross-fades between "Idle" and "Walk"; "Slash" sparks on each swing.
--
-- With `autopilot` on (scenes/autoplay.vscene) the player fights by itself: it walks the navmesh
-- to the nearest enemy (or to a health pickup when hurt) and swings when one is in reach.
local State = require("scripts/arena_state.luau")

local Player = {}
Player.speed = 5.5
Player.sprint = 1.5           -- speed multiplier while sprinting
Player.jump_speed = 6
Player.reach = 2.4            -- attack range (metres)
Player.arc = 0.35             -- cosine of the half-angle hit by a swing (~70 degrees)
Player.cooldown = 0.4         -- seconds between swings
Player.camera = "Main Camera" -- movement is relative to this camera
Player.autopilot = false
Player.heal_below = 55        -- autopilot fetches pickups below this health

function Player:start()
    self.robot = self.entity:find_child("Robot")
    self.slash = self.entity:find_child("Slash")
    self.cam = scene.find(self.camera)
    self.clip = "Idle"
    self.facing = vector.create(0, 0, -1)
    self.swing = 0
    self.path = nil
    self.repath = 0
end

function Player:save_state()
    return { facing = self.facing }
end

function Player:load_state(state)
    if state.facing then self.facing = state.facing end
end

-- Camera-relative movement: stick up walks away from the camera.
function Player:manual_move()
    local stick = input.vector("move_x", "move_y")
    local forward = vector.create(0, 0, -1)
    if self.cam and self.cam.valid then
        local d = self.entity.position - self.cam.position
        d = vector.create(d.x, 0, d.z)
        if vector.magnitude(d) > 0.01 then forward = vector.normalize(d) end
    end
    local right = vector.create(-forward.z, 0, forward.x)
    local move = forward * stick.y + right * stick.x
    local speed = self.speed * (if input.action("sprint") then self.sprint else 1)
    return move * speed, input.action_pressed("attack"), input.action_pressed("jump")
end

local ZERO = vector.create(0, 0, 0)
local function flat(v) return vector.create(v.x, 0, v.z) end

-- Closest live entity of `list` to `p` (nil when empty).
local function nearest(list, p)
    local best, best_d = nil, math.huge
    for _, e in list do
        if e.valid then
            local d = vector.magnitude(flat(e.position - p))
            if d < best_d then best, best_d = e, d end
        end
    end
    return best, best_d
end

-- Autopilot: pick a goal, follow a navmesh path to it, swing when an enemy is in reach.
function Player:auto_move(dt)
    local p = self.entity.position
    local enemy, enemy_d = nearest(State.enemies, p)
    local goal = enemy
    if State.health < self.heal_below and #State.pickups > 0 then
        local pickup, pickup_d = nearest(State.pickups, p)
        -- Fetch it unless an enemy is already in our face.
        if pickup and not (enemy and enemy_d < self.reach * 0.9 and pickup_d > 4) then goal = pickup end
    end
    if not goal then
        -- Between waves: wait in the middle of the arena.
        local to = flat(-p)
        if vector.magnitude(to) < 1 then return ZERO, false, false end
        return vector.normalize(to) * self.speed, false, false
    end
    local target = goal.position
    local attack = false
    if enemy and enemy_d < self.reach * 0.85 then
        -- Face the enemy and swing; step back slightly to keep it at reach.
        self.facing = vector.normalize(flat(enemy.position - p))
        attack = true
        if goal == enemy then return ZERO, attack, false end
    end
    self.repath -= dt
    if not self.path or self.repath <= 0 then
        self.path = nav.find_path(p, target)
        self.path_index = 2
        self.repath = 0.3
    end
    local waypoint = target
    if self.path then
        while self.path_index <= #self.path and vector.magnitude(flat(self.path[self.path_index] - p)) < 0.4 do
            self.path_index += 1
        end
        if self.path_index <= #self.path then waypoint = self.path[self.path_index] end
    end
    local to = flat(waypoint - p)
    if vector.magnitude(to) < 0.05 then return ZERO, attack, false end
    return vector.normalize(to) * self.speed, attack, false
end

function Player:update(dt)
    self.swing = math.max(0, self.swing - dt)
    if not State.ready or State.over then
        self.entity:move_character(ZERO)
        self:animate()
        return
    end
    local move, attack, jump
    if self.autopilot then
        move, attack, jump = self:auto_move(dt)
    else
        move, attack, jump = self:manual_move()
    end
    self.entity:move_character(move)
    if jump and self.entity.grounded then self.entity:jump(self.jump_speed) end
    if vector.magnitude(move) > 0.3 and not (self.autopilot and attack) then self.facing = vector.normalize(move) end
    if attack and self.swing <= 0 then
        self.swing = self.cooldown
        State.attack(self.entity.position + vector.create(0, 0.8, 0), self.facing, self.reach, self.arc)
        if self.slash then
            self.slash.position = self.facing * 1.1 + vector.create(0, 0.9, 0)
            self.slash:emit_particles(18)
        end
    end
    self:animate()
end

function Player:animate()
    if not self.robot then return end
    self.robot.rotation = vector.create(0, math.deg(math.atan2(self.facing.x, self.facing.z)), 0)
    local v = self.entity.velocity
    local moving = vector.magnitude(flat(v)) > 0.3 and self.entity.grounded
    local clip = if moving then "Walk" else "Idle"
    if clip ~= self.clip then
        self.clip = clip
        self.robot:play_animation(clip, 0.2)
    end
    -- Red flash while hurt.
    local hurt = State.ready and State.hurt > 0
    if hurt ~= self.flashing then
        self.flashing = hurt
        for _, part in State.body_parts(self.robot) do
            part:set_emissive(if hurt then vector.create(1, 0.1, 0.05) else nil, 2)
        end
    end
end

return Player
