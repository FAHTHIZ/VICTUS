-- Top-down hero on a CharacterController2D with `top_down` on: WASD, arrows or the left stick
-- walk in eight directions, sliding along the maze's tile colliders. Faces the way it walks.
--
-- With `autopilot` on (scenes/autoplay.vscene) the hero plays by itself: a breadth-first search
-- over the "Walls" tilemap finds the way to the nearest gem, then to the open exit, stepping
-- around slimes. The project's test uses it to play the whole dungeon headless.
local Hero = {}
Hero.speed = 4.5
Hero.sprint = 1.6 -- speed multiplier while "sprint" is held
Hero.autopilot = false

local EXIT_OPEN = 20

local function key(x, y) return x * 4096 + y end

function Hero:start()
    self.facing_left = false
    self.walls = scene.find("Walls")
    self.floor_map = scene.find("Floor")
    self.replan = 0
end

function Hero:face(dir_x)
    if math.abs(dir_x) <= 0.1 then return end
    local left = dir_x < 0
    if left == self.facing_left then return end
    self.facing_left = left
    local sp = self.entity:get("Sprite")
    sp.flip_x = left
    self.entity:set("Sprite", sp)
end

function Hero:fixed_update(dt)
    local dir
    if self.autopilot then
        dir = self:steer(dt)
    else
        dir = input.vector("move_x", "move_y")
    end
    local speed = self.speed * (input.action("sprint") and self.sprint or 1)
    self.entity:move_character(dir * speed)
    self:face(dir.x)
end

-- ---- autopilot ----------------------------------------------------------------------------

function Hero:goals()
    local goals = {}
    for _, e in scene.query("Sprite") do
        if e.name == "Gem" then
            local x, y = self.walls:world_to_tile(e.position)
            goals[key(x, y)] = true
        end
    end
    if next(goals) == nil then
        local x0, y0, x1, y1 = self.floor_map:tile_bounds()
        if x0 then
            for x = x0, x1 do
                for y = y0, y1 do
                    if self.floor_map:tile(x, y) == EXIT_OPEN then goals[key(x, y)] = true end
                end
            end
        end
    end
    return goals
end

-- Path (list of cells, excluding the start) to the nearest goal, avoiding `blocked` cells.
function Hero:search(sx, sy, goals, blocked)
    local x0, y0, x1, y1 = self.walls:tile_bounds()
    if not x0 then return nil end
    local came = { [key(sx, sy)] = false }
    local queue, head = { { sx, sy } }, 1
    while head <= #queue do
        local c = queue[head]
        head += 1
        if goals[key(c[1], c[2])] then
            local path, k = {}, key(c[1], c[2])
            while came[k] do
                table.insert(path, 1, { k // 4096, k % 4096 })
                k = came[k]
            end
            return path
        end
        for _, d in { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } do
            local nx, ny = c[1] + d[1], c[2] + d[2]
            local k = key(nx, ny)
            if nx >= x0 and ny >= y0 and nx <= x1 and ny <= y1 and came[k] == nil and not blocked[k]
                and self.walls:tile(nx, ny) == nil then
                came[k] = key(c[1], c[2])
                table.insert(queue, { nx, ny })
            end
        end
    end
    return nil
end

function Hero:steer(dt)
    local p = self.entity.position
    local hx, hy = self.walls:world_to_tile(p)
    self.replan -= dt
    if self.replan <= 0 or not self.path or #self.path == 0 then
        self.replan = 0.25
        local blocked = {}
        for _, e in scene.query("Sprite") do
            if e.name == "Slime" then
                local sx, sy = self.walls:world_to_tile(e.position)
                for _, d in { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } do
                    blocked[key(sx + d[1], sy + d[2])] = true
                end
            end
        end
        blocked[key(hx, hy)] = nil
        local goals = self:goals()
        self.path = self:search(hx, hy, goals, blocked) or self:search(hx, hy, goals, {}) or {}
    end
    -- Walk to the centre of the next cell; drop cells already reached.
    while #self.path > 0 do
        local c = self.path[1]
        local target = self.walls:tile_to_world(c[1], c[2])
        local to = vector.create(target.x - p.x, target.y - p.y, 0)
        if vector.magnitude(to) > 0.12 then return vector.normalize(to) end
        table.remove(self.path, 1)
    end
    -- At the goal cell: settle on its centre.
    local centre = self.walls:tile_to_world(hx, hy)
    local to = vector.create(centre.x - p.x, centre.y - p.y, 0)
    if vector.magnitude(to) > 0.05 then return vector.normalize(to) * 0.5 end
    return vector.create(0, 0, 0)
end

return Hero
