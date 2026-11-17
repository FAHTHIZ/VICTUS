-- HUD canvas: clicks on its buttons bubble up here as on_click(button, action). The
-- "Controls" button (or H) shows and hides the controls panel. When the HUD has a "Save Status"
-- toast, "quick_save" (F5, LB) writes a checkpoint of the whole scene to the "quick" slot and
-- "quick_load" (F9, RB) restores it; every script's save_state/load_state carries its own data.
local Hud = {}
Hud.slot = "quick"
Hud.toast_seconds = 2

function Hud:start()
    self.toast = 0
end

function Hud:notify(text)
    local t = scene.find("Save Status")
    if not t then return end
    t:set_text(text)
    t:set_visible(true)
    self.toast = self.toast_seconds
end

function Hud:quick_save()
    local ok, err = save.checkpoint(self.slot, "Quick save")
    self:notify(if ok then "Game saved" else "Save failed: " .. tostring(err))
end

function Hud:quick_load()
    if not save.exists(self.slot) then
        self:notify("No quick save yet (F5 saves)")
        return
    end
    local ok, err = save.restore(self.slot) -- the scene is replaced at the end of this frame
    if not ok then self:notify("Load failed: " .. tostring(err)) end
end

-- Checkpoint hooks: the restored HUD confirms the load.
function Hud:save_state()
    return { saved = true }
end

function Hud:load_state(state)
    self:notify("Game loaded")
end

function Hud:toggle_help()
    local panel = scene.find("Controls Panel")
    if panel then panel:set_visible(not panel:visible()) end
end

function Hud:on_click(button, action)
    if action == "toggle_help" then self:toggle_help() end
end

function Hud:update(dt)
    if input.action_pressed("help") then self:toggle_help() end
    if scene.find("Save Status") then
        if input.action_pressed("quick_save") then self:quick_save() end
        if input.action_pressed("quick_load") then self:quick_load() end
    end
    if self.toast > 0 then
        self.toast -= dt
        if self.toast <= 0 then
            local t = scene.find("Save Status")
            if t then t:set_visible(false) end
        end
    end
end

return Hud
