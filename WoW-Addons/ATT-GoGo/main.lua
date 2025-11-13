local addonName = ...
local ICON_FILE = "Interface\\AddOns\\ATT-GoGo\\icon-Go2.tga"
TITLE = GetAddOnMetadata(addonName, "Title") or "UNKNOWN"
CTITLE = "|cff00ff00" .. TITLE .. "|r "

local frame = CreateFrame("Frame")
frame:RegisterEvent("ADDON_LOADED")

local function PrintStartup()
    local version = GetAddOnMetadata(addonName, "Version")
    local author = GetAddOnMetadata(addonName, "Author")
    local coauthor = GetAddOnMetadata(addonName, "X-CoAuthor")
    print(CTITLE .. "v" .. version .. ", vibed by: " .. author .. " & " .. coauthor)
end

local function OpenUncollectedForHere()
    local target = Util.ResolvePopupTargetForCurrentContext()
    if target then
        ShowUncollectedPopup(target)
    else
        TP()
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
    tooltip:AddLine(CTITLE, 0, 1, 0)
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
        text = TITLE,
        icon = ICON_FILE,
        OnClick = function(self, button)
            if button == "LeftButton" then
                if IsShiftKeyDown() then
                    OptionsUI.Show()
                elseif IsAltKeyDown() then -- toggle LUA errors' window
                    local en = GetCVarBool("scriptErrors") and "0" or "1"
                    print(CTITLE .. "setting scriptErrors to " .. en)
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

-- prolonged trash-combat warning (dungeons/raids, solo only, non-boss)
local function SetupTrashCombatWarning()
  local DELAY = 50
  local f = CreateFrame("Frame")
  local ticket = 0
  local startedAt

  local function InDungeon()
    local inInst, typ = IsInInstance()
    return inInst and (typ == "party" or typ == "raid")
  end

  local function IsSolo() return GetNumGroupMembers() == 0 end

  local function Finish()
    ticket = ticket + 1
    startedAt = nil
  end

  local function Fire(myTicket)
    if myTicket ~= ticket then return end
    if InDungeon() and IsSolo() and not IsEncounterInProgress() and UnitAffectingCombat("player") then
      local elapsed = startedAt and (GetTime() - startedAt) or 0
      RaidNotice_AddMessage(RaidWarningFrame, "Trash combat > 50s — empower in ~10s!", ChatTypeInfo.RAID_WARNING)
      PlaySound(SOUNDKIT.RAID_WARNING, "Master")
      print(CTITLE .. "Non-boss combat > 50s — finish or reset")
    end
  end

  local function Start()
    if not InDungeon() or not IsSolo() or IsEncounterInProgress() then return end
    startedAt = GetTime()
    ticket = ticket + 1
    local myTicket = ticket
    C_Timer.After(DELAY, function() Fire(myTicket) end)
  end

  f:SetScript("OnEvent", function(_, e, ...)
    if e == "PLAYER_REGEN_DISABLED" then
      Start()
    elseif e == "PLAYER_REGEN_ENABLED" or e == "ENCOUNTER_START" then
      Finish()
    elseif e == "ENCOUNTER_END" then
      if UnitAffectingCombat("player") then Start() end
    elseif e == "PLAYER_ENTERING_WORLD" or e == "ZONE_CHANGED_NEW_AREA" then
      Finish()
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
local THRESHOLD   = 50
local BATCH_DELAY = 0.40 -- wait this long after the *last* event

local collectedBatch = { count = 0, timer = nil }

local function FlushCollectedBatch()
    local cnt = collectedBatch.count
    collectedBatch.count, collectedBatch.timer = 0, nil

    if cnt >= THRESHOLD then
    local perf1 = AGGPerf.auto("FlushCollectedBatch:BigWave")
        -- Big wave => assume whole-DB refresh; rebuild everything
        DebugLog("BIG wave, cnt = " .. cnt)
        Util.InvalidateProgressCache()
        Util.InvalidateMapProgress()
        SetupMainUI()           -- full rebuild of main frame widgets (also refreshes data)
    perf1()
    else
        -- Small wave => do a context snapshot + popup/active-tab refresh
        local node, info = Util.ResolveContextNode()
        if info.kind == "instance" then
        local perf3 = AGGPerf.auto("FlushCollectedBatch:SmallWave:instance")
            -- Invalidate the currently relevant difficulty child (and parents)
            local curDiff = ATT.GetCurrentDifficultyID()
            local child = Util.SelectDifficultyChild(node, curDiff) or node
            Util.InvalidateProgressCache(child)
        perf3()
        else
            -- Zone context: just nuke this map’s memo row
            if info.uiMapID then Util.InvalidateMapProgress(info.uiMapID) else TP(node, info) end
        end

        Util.SaveCurrentContextProgress()
        RefreshUncollectedPopupForContextIfShown(true)
        RefreshActiveTab()
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
    print("alternatively you can use /agg or /attgogo")
end

local function test()
  local ctx = Util.ResolvePopupTargetForCurrentContext() -- provides instance per-difficulty subset
  local mapID = C_Map.GetBestMapForUnit("player")
  local pkg = ATT.GetCachedDataForMapID(mapID)          -- always provides a combined set
  print("mapID: " .. mapID)

  local function nt(o)
    return ("name=%s; text=%s"):format(tostring(o and o.name or "noname"), tostring(o and o.text or "notext"))
  end

  -- ctx progress straight from the node
  local c1, t1 = Util.ATTGetProgress(ctx)
  local ctx_txt = nt(ctx) .. ("; %d/%d"):format(c1 or 0, t1 or 0)

  -- pckg progress via the map root wrapper
  local c2, t2 = Util.ResolveMapProgress(mapID)
  local pkg_txt = nt(pkg) .. ("; %d/%d"):format(c2 or 0, t2 or 0)

  if ctx_txt ~= pkg_txt then
    print("ctx: " .. ctx_txt)
    print("pkg: " .. pkg_txt)
  end

  if IsInInstance() then
    local name, instType, difficultyID, difficultyName, maxPlayers, dynDifficulty, isDyn, instMapID, grpSize = GetInstanceInfo()
    print(name .. ", " .. instType .. ", difficulty " .. difficultyID .. " (" .. difficultyName .. "), max players " .. maxPlayers .. ", mapID " .. instMapID .. ", group size " .. grpSize)
  end
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
        local PERF    = { p = true, perf = true }
        local TEST    = { t = true, test = true }

        if HELP[cmd]    then PrintSlashCmdHelp()        return end
        if OPTIONS[cmd] then OptionsUI.Show()           return end
        if SHOW[cmd]    then ShowMainFrame()            return end
        if LIST[cmd]    then OpenUncollectedForHere()   return end
        if DUMP[cmd]    then DumpCurrentCtx()           return end
        if TEST[cmd]    then test()                     return end
        if PERF[cmd]    then AGGPerf.on(rest == "1")    return end
        if cmd == "add" then DebugLog(rest)             return end

        print(CTITLE .. "Unknown command. Type '/gogo help' for options.")
    end
end

frame:SetScript("OnEvent", function(self, event, arg1)
    if arg1 ~= addonName then return end

    -- Ensure DB tables exist
    ATTGoGoCharDB = ATTGoGoCharDB or {}
    ATTGoGoDB = ATTGoGoDB or {}
    ATTGoGoDB.minimap = ATTGoGoDB.minimap or { minimapPos = 128, hide = false }

    Debug_Init()
    AGGPerf.on(true)

    -- === Wait for ATT ("All The Things") ===
    ATT.AddEventHandler("OnReady", function()
        Util.CanonicalizePopupIdFilters()
        SetupMainUI()
        SetupMinimapIcon()
        EnsurePreviewDock(); EnsurePopup() -- create the preview dock before the uncollected list popup that uses it
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
--            test()
            -- slight delay so C_Map / GetInstanceInfo settle
            C_Timer.After(0.15, RefreshUncollectedPopupForContextIfShown)
        end)

        local bossWatcher = CreateFrame("Frame")
        bossWatcher:RegisterEvent("BOSS_KILL")
        bossWatcher:RegisterEvent("ENCOUNTER_END")
        bossWatcher:SetScript("OnEvent", OnThingCollected)

        ATT.AddEventHandler("OnThingCollected", OnThingCollected)

        PrintStartup()
        AGGPerf.on(true)
    end)
end)

