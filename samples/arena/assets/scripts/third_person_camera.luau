-- Third-person orbit camera: stays `distance` behind and above `target` at `pitch` degrees,
-- turns with the "look_x" action (right stick) or the mouse while the right button is held,
-- and pulls in when a wall or pillar is between it and the player (physics raycast).
local Camera = {}
Camera.target = "Player"
Camera.distance = 9
Camera.pitch = 32        -- degrees above the horizon
Camera.yaw = 0           -- degrees; 0 looks down -Z
Camera.turn_speed = 120  -- degrees per second at full stick
Camera.mouse_speed = 0.25 -- degrees per pixel
Camera.height = 1.0      -- look-at point above the target's origin
Camera.smoothing = 8
Camera.probe = 0.5      -- half-width of the occlusion probe (metres)

function Camera:start()
    self.subject = scene.find(self.target)
    self.current = nil
end

function Camera:update(dt)
    if not self.subject or not self.subject.valid then return end
    self.yaw += input.value("look_x") * self.turn_speed * dt
    if input.mouse_down(2) then self.yaw += input.mouse_delta().x * self.mouse_speed end
    local yaw, pitch = math.rad(self.yaw), math.rad(self.pitch)
    local back = vector.create(math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
    local focus = self.subject.position + vector.create(0, self.height, 0)
    local distance = self.distance
    -- Three rays (centre and either side) so a pillar the thin centre ray would graze still pulls
    -- the camera in; mask 1 = layer 0 only, so robots never block the view.
    local side = vector.create(math.cos(yaw), 0, -math.sin(yaw)) * self.probe
    for _, offset in { vector.create(0, 0, 0), side, -side } do
        local hit = physics.raycast(focus + offset, back, distance, 1, self.subject)
        if hit then distance = math.max(1.5, hit.distance - 0.3) end
    end
    local goal = focus + back * distance
    if not self.current then self.current = goal end
    local k = 1 - math.exp(-self.smoothing * dt)
    self.current = self.current + (goal - self.current) * k
    self.entity.position = self.current
    self.entity:look_at(focus)
end

return Camera
