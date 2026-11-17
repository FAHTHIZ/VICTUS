-- Shared state of one arena match. `require("scripts/arena_state.luau")` returns the same table
-- to every script of the scene, so the arena rules, the player and the camera talk through it:
--   the player queues attacks, the arena resolves them against its enemies;
--   the arena owns the player's health and publishes the live enemies and pickups.
-- The arena resets it in start(), so a reload or checkpoint restore begins clean.
local State = {}

function State.reset(max_health)
    State.ready = true
    State.max_health = max_health
    State.health = max_health
    State.attacks = {}  -- { origin, facing, range, arc } queued by the player this frame
    State.enemies = {}  -- live enemy entities (for the autopilot)
    State.pickups = {}  -- live health pickups (for the autopilot)
    State.over = false  -- won or lost: the player stops
    State.hurt = 0      -- seconds of hit flash left on the player
end

-- Queues a melee swing from `origin` along the unit vector `facing`.
function State.attack(origin, facing, range, arc)
    if not State.ready or State.over then return end
    table.insert(State.attacks, { origin = origin, facing = facing, range = range, arc = arc })
end

State.ready = false
return State
