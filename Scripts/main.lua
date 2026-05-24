-- SN2ThirdPersonMod  v1.1
-- Toggle Subnautica 2's built-in third-person camera.
--
-- Default keybind: F3  (change via SN2ThirdPersonSettings ImGui tab, or edit settings.ini)
-- Open settings UI:    Ctrl+O  (UE4SS window)
-- Hot-reload:          Ctrl+R  (applies keybind changes)

local UEHelpers = require("UEHelpers")

-- ── Defaults (overridden by settings.ini) ────────────────────────────────────

local _KEY      = "F3"
local _hideHUD  = true

-- ── Settings file (written by the SN2ThirdPersonSettings C++ mod) ────────────
-- Path is relative to the game executable (Win64/).

local _SETTINGS_PATH = "ue4ss/Mods/SN2ThirdPersonMod/settings.ini"

local function LoadSettings()
    local f = io.open(_SETTINGS_PATH, "r")
    if not f then return end
    for line in f:lines() do
        local k, v = line:match("^(%w+)=(.+)$")
        if k == "ToggleKey" and v then _KEY     = v               end
        if k == "HideHUD"   and v then _hideHUD = (v == "true")   end
    end
    f:close()
end

LoadSettings()

-- ── State ─────────────────────────────────────────────────────────────────────

local _active    = false
local _maskActor = nil

-- ── Logging ───────────────────────────────────────────────────────────────────

local function Log(msg)
    print("[ThirdPerson] " .. tostring(msg) .. "\n")
end

-- ── Helpers ───────────────────────────────────────────────────────────────────

local function GetPC()
    local ok, pc = pcall(function() return UEHelpers.GetPlayerController() end)
    if ok and pc and pc:IsValid() then return pc end
    return nil
end

-- ── Mask / HUD overlay ───────────────────────────────────────────────────────
-- BP_SN2WorldHUD_C confirmed via live testing. SetActorHiddenInGame hides the
-- entire HUD. Isolating only the scuba mask requires Blueprint asset unpacking.

local _HUD_CANDIDATES = {
    "BP_SN2WorldHUD_C",
    "WBP_HUD_C",
    "WBP_PlayerHUD_C",
    "WBP_DiveMask_C",
    "WBP_Helmet_C",
    "WBP_ScubaMask_C",
    "WBP_FPOverlay_C",
    "WBP_PlayerOverlay_C",
}

local function FindMaskActor()
    if _maskActor then
        local ok, valid = pcall(function() return _maskActor:IsValid() end)
        if ok and valid then return _maskActor end
        _maskActor = nil
    end

    for _, cn in ipairs(_HUD_CANDIDATES) do
        local inst = FindFirstOf(cn)
        if inst then
            local vok, valid = pcall(function() return inst:IsValid() end)
            if vok and valid then
                _maskActor = inst
                Log("HUD actor found: " .. cn)
                return inst
            end
        end
    end

    local pc = GetPC()
    if pc then
        local ok, hud = pcall(function() return pc:GetHUD() end)
        if ok and hud then
            local vok, valid = pcall(function() return hud:IsValid() end)
            if vok and valid then
                _maskActor = hud
                Log("HUD actor found via GetHUD()")
                return hud
            end
        end
    end

    Log("HUD actor not found — overlay will remain visible")
    return nil
end

local function SetHUDVisible(visible)
    local actor = FindMaskActor()
    if not actor then return end
    local ok = pcall(function() actor:SetActorHiddenInGame(not visible) end)
    if not ok then
        pcall(function() actor:SetVisibility(visible and 0 or 2) end)
    end
    Log("HUD: " .. (visible and "shown" or "hidden"))
end

-- ── Toggle ────────────────────────────────────────────────────────────────────

local function Toggle()
    -- Re-read settings each toggle so HideHUD changes from the ImGui panel
    -- take effect immediately without a hot-reload.
    LoadSettings()

    local pc = GetPC()
    if not pc then Log("No player controller"); return end

    local ok = pcall(function() pc:ToggleThirdPerson() end)
    if not ok then Log("ToggleThirdPerson() failed"); return end

    _active = not _active

    if _active then
        if _hideHUD then SetHUDVisible(false) end
        Log("Third-person ON  (HideHUD=" .. tostring(_hideHUD) .. ")")
    else
        SetHUDVisible(true)
        Log("Third-person OFF")
    end
end

-- ── Keybind ───────────────────────────────────────────────────────────────────

RegisterKeyBind(Key[_KEY], function()
    ExecuteInGameThread(Toggle)
end)

-- ── Settings screen detection (writes flag for C++ overlay) ─────────────────
-- The C++ SN2ThirdPersonSettings mod reads this file to show/hide its ImGui overlay.

local _SETTINGS_FLAG = "ue4ss/Mods/SN2ThirdPersonMod/settings_open.flag"

local function WriteSettingsFlag(val)
    local f = io.open(_SETTINGS_FLAG, "w")
    if f then f:write(val) f:close() end
end

-- Initialise to closed
WriteSettingsFlag("0")

-- Debug log for hook investigation
local _LUA_DEBUG = "ue4ss/Mods/SN2ThirdPersonMod/lua_hooks.log"
local function LuaDebug(msg)
    local f = io.open(_LUA_DEBUG, "a")
    if f then f:write(msg .. "\n") f:close() end
end
LuaDebug("=== Lua mod loaded, registering hooks ===")

pcall(function()
    local ok, err = pcall(function()
        RegisterHook("/Script/CommonUI.CommonActivatableWidget:BP_OnActivated", function(Context)
            -- Log ALL activations, not just Settings, to confirm hook fires
            local nameOk, name = pcall(function() return Context:GetClass():GetName() end)
            local className = nameOk and name or "ERROR_GETCLASS"
            LuaDebug("BP_OnActivated: " .. className)
            if className:find("Settings", 1, true) then
                WriteSettingsFlag("1")
            end
        end)
    end)
    if not ok then LuaDebug("RegisterHook BP_OnActivated FAILED: " .. tostring(err)) end
end)

pcall(function()
    local ok, err = pcall(function()
        RegisterHook("/Script/CommonUI.CommonActivatableWidget:BP_OnDeactivated", function(Context)
            local nameOk, name = pcall(function() return Context:GetClass():GetName() end)
            local className = nameOk and name or "ERROR_GETCLASS"
            LuaDebug("BP_OnDeactivated: " .. className)
            if className:find("Settings", 1, true) then
                WriteSettingsFlag("0")
            end
        end)
    end)
    if not ok then LuaDebug("RegisterHook BP_OnDeactivated FAILED: " .. tostring(err)) end
end)

-- Poll IsActivated() via ExecuteWithDelay loop (avoids broken GameEngine:Tick hook)
local function PollSettings()
    local widget = FindFirstOf("WBP_Settings2Screen_C")
    if widget and widget:IsValid() then
        local ok, result = pcall(function() return widget:IsActivated() end)
        LuaDebug("poll: widget found  IsActivated ok=" .. tostring(ok) .. " result=" .. tostring(result))
        local flagVal = (ok and result == true) and "1" or "0"
        WriteSettingsFlag(flagVal)
    else
        WriteSettingsFlag("0")
    end
    ExecuteWithDelay(300, PollSettings)
end
ExecuteWithDelay(2000, PollSettings)  -- start 2s after mod load

-- ── Level-load cleanup ────────────────────────────────────────────────────────

local _LOAD_HOOKS = {
    "/Script/Engine.GameEngine:LoadMap",
    "/Script/Engine.GameEngine:SeamlessTravel",
}
for _, path in ipairs(_LOAD_HOOKS) do
    pcall(function()
        RegisterHook(path, function()
            _active    = false
            _maskActor = nil
            SetHUDVisible(true)
            WriteSettingsFlag("0")
        end)
    end)
end

Log(string.format("SN2ThirdPersonMod ready — key=%s  hideHUD=%s  (Ctrl+O for settings UI)",
    _KEY, tostring(_hideHUD)))
