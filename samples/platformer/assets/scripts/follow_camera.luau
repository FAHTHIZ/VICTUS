-- Keeps the camera on the entity named `target` (a little ahead and above it), easing towards it.
local Follow = {}
Follow.target = "Hero"
Follow.sharpness = 6
Follow.lead = 2.5   -- metres ahead of the target in its running direction
Follow.height = 1.5 -- metres above the target
Follow.min_y = 4    -- never shows much below the level floor

function Follow:update(dt)
    local t = scene.find(self.target)
    if not t then return end
    local p, goal = self.entity.position, t.position
    local vx = t.velocity.x
    local ahead = vx > 0.5 and self.lead or (vx < -0.5 and -self.lead or 0)
    local gx, gy = goal.x + ahead, math.max(goal.y + self.height, self.min_y)
    local k = 1 - math.exp(-self.sharpness * dt)
    self.entity.position = vector.create(p.x + (gx - p.x) * k, p.y + (gy - p.y) * k, p.z)
end

return Follow
