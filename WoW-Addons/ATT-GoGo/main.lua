local addonName, addonTable = ...
local ICON_FILE = "Interface\\AddOns\\ATT-GoGo\\icon-Go2.tga"
title = GetAddOnMetadata(addonName, "Title") or "UNKNOWN"
local CTITLE = "|cff00ff00[" .. title .. "]|r "

local frame = CreateFrame("Frame")
frame:RegisterEvent("ADDON_LOADED")

local function PrintStartup()
    local version = GetAddOnMetadata(addonName, "Version")
    local author = GetAddOnMetadata(addonName, "Author")
    local coauthor = GetAddOnMetadata(addonName, "X-CoAuthor")
    print(CTITLE .. version .. " |cffffff00Vibed by:|r " .. author .. " & " .. coauthor)
end

local function OpenUncollectedForHere()
    local target = Util.ResolvePopupTargetForCurrentContext()
    if target then
        ShowUncollectedPopup(target)
    else
        TP(node, info)
        print(CTITLE .. "Nothing to show for this location.")
    end
end

-- Refresh the Uncollected popup to the *current* context, if visible and allowed.
-- @param force boolean (optional) - force rebuild even if current node didn't change
local function RefreshUncollectedPopupForContextIfShown(force)
    local popup = _G.ATTGoGoUncollectedPopup -- gets created in EnsurePopup() on first use
    if not popup:IsShown() then return end

    if not (force or GetSetting("autoRefreshPopupOnZone", true)) then return end

    local target = Util.ResolvePopupTargetForCurrentContext()
    if target and (force or popup.currentData ~= target) then
      ShowUncollectedPopup(target)
    end

end

local function ShowMinimapTooltip(tooltip)
    RequestRaidInfo()
    tooltip:AddLine(title, 0, 1, 0)
    tooltip:AddLine("Left-click: Open main grid window", 1, 1, 1)
    tooltip:AddLine("Right-click: Uncollected for current instance/zone", 1, 1, 1)
    tooltip:AddLine("Shift-click: Open options", 1, 1, 1)
    tooltip:AddLine("Left-drag: Move icon", 1, 1, 1)
    Tooltip.AddContextProgressTo(tooltip)
end

local function SetupMinimapIcon()
    local ldb = LibStub:GetLibrary("LibDataBroker-1.1", true)
    local icon = LibStub:GetLibrary("LibDBIcon-1.0", true)
    local dataObj = ldb:NewDataObject(addonName, {
        type = "data source",
        text = title,
        icon = ICON_FILE,
        OnClick = function(self, button)
            if button == "LeftButton" then
                if IsShiftKeyDown() then
                    OptionsUI.Show()
                elseif IsAltKeyDown() then -- toggle LUA errors' window
                    local en = GetCVarBool("scriptErrors") and "0" or "1"
                    print(CTITLE .. "Setting scriptErrors to " .. en)
                    SetCVar("scriptErrors", en)
                else
                    ShowMainFrame()
                end
            elseif button == "RightButton" then
                OpenUncollectedForHere()
            end
        end,
        OnTooltipShow = ShowMinimapTooltip,
    })
    icon:Register(addonName, dataObj, ATTGoGoDB.minimap)
end

-- Trash-Combat 50s Warning (dungeons only, non-boss)
local function SetupTrashCombatWarning()
  local DELAY = 50
  local f = CreateFrame("Frame")
  local ticket = 0
  local startedAt

  local function InDungeon()
    local inInst, typ = IsInInstance()
    return inInst and typ == "party"
  end

  local function Cancel()
    ticket = ticket + 1
    startedAt = nil
  end

  local function Fire(myTicket)
    if myTicket ~= ticket then return end
    if InDungeon() and not IsEncounterInProgress() and UnitAffectingCombat("player") then
      local elapsed = startedAt and (GetTime() - startedAt) or 0
      RaidNotice_AddMessage(RaidWarningFrame, "Trash combat > 50s — empower at ~60s!", ChatTypeInfo.RAID_WARNING)
      PlaySound(SOUNDKIT.RAID_WARNING, "Master")
      print("|cffff7e40ATT-GoGo:|r Non-boss combat > 50s — finish or reset. (elapsed "..math.floor(elapsed).."s)")
    end
  end

  local function Start()
    if not InDungeon() or IsEncounterInProgress() then return end
    startedAt = GetTime()
    ticket = ticket + 1
    local myTicket = ticket
    C_Timer.After(DELAY, function() Fire(myTicket) end)
  end

  f:SetScript("OnEvent", function(_, e, ...)
    if e == "PLAYER_REGEN_DISABLED" then
      Start()
    elseif e == "PLAYER_REGEN_ENABLED" or e == "ENCOUNTER_START" then
      Cancel()
    elseif e == "ENCOUNTER_END" then
      if UnitAffectingCombat("player") then Start() end
    elseif e == "PLAYER_ENTERING_WORLD" or e == "ZONE_CHANGED_NEW_AREA" then
      Cancel()
    end
  end)

  f:RegisterEvent("PLAYER_REGEN_DISABLED")
  f:RegisterEvent("PLAYER_REGEN_ENABLED")
  f:RegisterEvent("ENCOUNTER_START")
  f:RegisterEvent("ENCOUNTER_END")
  f:RegisterEvent("PLAYER_ENTERING_WORLD")
  f:RegisterEvent("ZONE_CHANGED_NEW_AREA")
end

----------------------------------------------------------------
-- Batch ATT "OnThingCollected" updates (simple version)
----------------------------------------------------------------
local THRESHOLD   = 2
local BATCH_DELAY = 0.40 -- wait this long after the *last* event

local collectedBatch = { count = 0, timer = nil }
local smallWaveTimer = nil     -- one-shot guard for the delayed save

local function FlushCollectedBatch()
    local cnt = collectedBatch.count
    collectedBatch.count, collectedBatch.timer = 0, nil

    if cnt >= THRESHOLD then
        -- Big wave => assume whole-DB refresh; rebuild everything
        SetupMainUI()           -- full rebuild of main frame widgets (also refreshes data)
    else
        -- Small wave => do a delayed context snapshot + popup/active-tab refresh
        if smallWaveTimer then
            smallWaveTimer:Cancel()
        end
        if not C_Map.GetBestMapForUnit("player") then TP("no *location* available for player") end
        local delay = C_Map.GetBestMapForUnit("player") and 0 or 2 -- extra settle time before context snapshot, if needed
        smallWaveTimer = C_Timer.NewTimer(delay, function()
            smallWaveTimer = nil
            Util.SaveCurrentContextProgress()
            RefreshUncollectedPopupForContextIfShown(true)
            RefreshActiveTab()
        end)
    end
end

local function OnThingCollected(data, etype)
    collectedBatch.count = collectedBatch.count + 1

    -- Silence-based debounce: restart a cancelable timer each event
    if collectedBatch.timer then
        collectedBatch.timer:Cancel()
    end
    collectedBatch.timer = C_Timer.NewTimer(BATCH_DELAY, FlushCollectedBatch)
end

local function DumpCurrentCtx()
  local target = Util.ResolvePopupTargetForCurrentContext()
  print(CTITLE .. "dump → " .. (Util.NodeDisplayName(target) or "?"))
  DebugPrintNodePath(target)
  DebugRecursive(target, target.name or target.text, 0, 3, false)
end

SLASH_ATTGOGO1 = "/attgogo"
SLASH_ATTGOGO2 = "/gogo"
SLASH_ATTGOGO3 = "/agg"

local function PrintSlashCmdHelp()
    print(CTITLE .. "Commands")
    print("/gogo help        - Show this help")
    print("/gogo options     - Open the options window")
    print("/gogo show        - Show the main window")
    print("/gogo list        - Open Uncollected for current instance/zone")
--    print("/gogo dump        - Debug: path + recursive dump for current context")
--    print("/gogo add <text>  - Append <text> into ATT-GoGo debug log")
    print("alternatively you can use /attgogo")
end

local function SetupSlashCmd()
    SlashCmdList["ATTGOGO"] = function(msg)
        local raw = (msg or ""):trim()
        local cmd, rest = raw:match("^(%S+)%s*(.*)$")
        cmd = (cmd or ""):lower()

        local HELP    = { h = true, help = true, ["?"] = true, [""]  = true }
        local OPTIONS = { o = true, options = true }
        local SHOW    = { s = true, show = true }
        local LIST    = { l = true, list = true }
        local DUMP    = { d = true, dump = true }

        if HELP[cmd]    then PrintSlashCmdHelp()        return end
        if OPTIONS[cmd] then OptionsUI.Show()           return end
        if SHOW[cmd]    then ShowMainFrame()            return end
        if LIST[cmd]    then OpenUncollectedForHere()   return end
        if DUMP[cmd]    then DumpCurrentCtx()           return end
        if cmd == "add" then DebugLog(rest)             return end

        print(CTITLE .. "Unknown command. Type '/attgogo help' for options.")
    end
end

frame:SetScript("OnEvent", function(self, event, arg1)
    if arg1 ~= addonName then return end

    -- Ensure DB tables exist
    ATTGoGoCharDB = ATTGoGoCharDB or {}
    ATTGoGoDB = ATTGoGoDB or {}
    ATTGoGoDB.minimap = ATTGoGoDB.minimap or { minimapPos = 128, hide = false }

    PrintStartup()
    Debug_Init()

    -- === Wait for ATT ("All The Things") ===
    ATT.AddEventHandler("OnReady", function()
        Util.CanonicalizePopupIdFilters()
        SetupMainUI()
        SetupMinimapIcon()
        EnsurePreviewDock() -- create the preview dock before the uncollected list popup that uses it
        EnsurePopup()
        OptionsUI.Init()
        SetupSlashCmd()
        SetupTrashCombatWarning()

        -- Auto-refresh Uncollected popup on zone/instance changes (if enabled)
        local zoneWatcher = CreateFrame("Frame")
        for _, ev in ipairs({
            "PLAYER_ENTERING_WORLD",
            "ZONE_CHANGED",
            "ZONE_CHANGED_INDOORS",
            "ZONE_CHANGED_NEW_AREA",
            "UPDATE_INSTANCE_INFO",
        }) do
            zoneWatcher:RegisterEvent(ev)
        end
        zoneWatcher:SetScript("OnEvent", function()
            -- slight delay so C_Map / GetInstanceInfo settle
            C_Timer.After(0.15, RefreshUncollectedPopupForContextIfShown)
        end)

        local bossWatcher = CreateFrame("Frame")
        bossWatcher:RegisterEvent("BOSS_KILL")
        bossWatcher:RegisterEvent("ENCOUNTER_END")
        bossWatcher:SetScript("OnEvent", OnThingCollected)

        ATT.AddEventHandler("OnThingCollected", OnThingCollected)

        print(CTITLE .. "is ready")
    end)
end)

