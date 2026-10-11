-- Applied by the Omarchy plugin after HyprCapture loads and after every config
-- reload. Copy this file and omarchy/bindings.json into
-- ~/.config/hyprcapture/omarchy.lua to take full control of both.
if not (hl.plugin and hl.plugin.hyprcapture) then return end

-- Keep the settings people pick in the toolbar between captures.
hl.config({ plugin = { hyprcapture = { remember_settings = true } } })
