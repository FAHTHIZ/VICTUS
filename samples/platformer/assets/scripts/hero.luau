-- Side-view hero on a CharacterController2D: A/D, arrows or the left stick run, Space / W / Up
-- (or the A button) jump, S / Down drops through wooden planks. Shows idle, run and jump frames.
--
-- With `autopilot` on (scenes/autoplay.vscene) the hero plays by itself: it runs right and
-- jumps at walls, gaps, spikes and beetles it senses in the "Ground" and "Decor" tilemaps. The
-- project's test uses it to play all three levels headless.
local Hero = {}
Hero.speed = 6
Hero.jump_speed = 12
Hero.autopilot = false
Hero.jump_sound = "audio/jump.wav"
Hero.texture = "textures/ruin_tiles.png"

local IDLE, RUN, JUMP = 8, 9, 10
local PLANK, SPIKES = 3, 4
local COLUMNS, ROWS = 4, 4

function Hero:start()
    self.ground = scene.find("Ground")
    self.decor = scene.find("Decor")
    self.facing_left = false
    self.frame = -1
    self.run_time = 0
    self.stuck = 0
end

function Hero:show(frame, left)
    if frame == self.frame and left == self.facing_left then return end
    self.frame, self.facing_left = frame, left
    local c, r = frame % COLUMNS, frame // COLUMNS
    self.entity:set("Sprite", { size = { 1, 1 }, layer = 4, uv = { c / COLUMNS, r / ROWS, (c + 1) / COLUMNS, (r + 1) / ROWS },
        flip_x = left })
    self.entity:set_sprite(self.texture)
end

function Hero:fixed_update(dt)
    local e = self.entity
    local run, jump, drop
    if self.autopilot then
        run, jump = self:steer(dt)
        drop = false
    else
        run = input.value("move_x")
        local vertical = input.value("move_y")
        jump = input.action("jump") or vertical >= 0.5
        drop = vertical <= -0.5
    end
    e:move_character(vector.create(run * self.speed, 0, 0))
    if drop and e.grounded then
        e:drop_through()
    elseif jump and e:jump(self.jump_speed) then
        audio.play(self.jump_sound, { volume = 0.6 })
    end
    -- Animation frame: jump in the air, alternate run/idle while running.
    local left = self.facing_left
    if math.abs(run) > 0.1 then left = run < 0 end
    local frame = IDLE
    if not e.grounded then
        frame = JUMP
    elseif math.abs(run) > 0.1 then
        self.run_time += dt
        frame = (math.floor(self.run_time * 8) % 2 == 0) and RUN or IDLE
    end
    self:show(frame, left)
end

-- ---- autopilot ----------------------------------------------------------------------------

function Hero:solid(x, y)
    local t = self.ground:tile(x, y)
    return t ~= nil and t ~= PLANK
end

-- Returns the run direction and whether to jump this step.
function Hero:steer(dt)
    local e = self.entity
    local p = e.position
    if not e.grounded then return 1, false end
    local _, y = self.ground:world_to_tile(p)
    local function column(dx) return (self.ground:world_to_tile(vector.create(p.x + dx, p.y, 0))) end
    local near, far = column(0.75), column(1.4)
    local wall = self:solid(near, y) or self:solid(near, y + 1)
    local gap = self.ground:tile(near, y - 1) == nil
    local spikes = self.decor:tile(near, y) == SPIKES or self.decor:tile(far, y) == SPIKES
    local beetle = false
    for _, b in scene.query("Sprite") do
        if b.name == "Beetle" then
            local d = b.position - p
            if d.x > 0.2 and d.x < 2.6 and math.abs(d.y) < 1 then beetle = true end
        end
    end
    -- Not moving although running: jump out of whatever holds the hero.
    if math.abs(e.velocity.x) < 0.2 then self.stuck += dt else self.stuck = 0 end
    local stuck = self.stuck > 0.4
    if stuck then self.stuck = 0 end
    return 1, wall or gap or spikes or beetle or stuck
end

return Hero
