-- Keeps the camera on the entity named `target`, easing towards it.
local Follow = {}
Follow.target = "Hero"
Follow.sharpness = 8

function Follow:update(dt)
    local t = scene.find(self.target)
    if not t then return end
    local p, goal = self.entity.position, t.position
    local k = 1 - math.exp(-self.sharpness * dt)
    self.entity.position = vector.create(p.x + (goal.x - p.x) * k, p.y + (goal.y - p.y) * k, p.z)
end

return Follow
