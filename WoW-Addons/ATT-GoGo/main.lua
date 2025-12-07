local addonName = ...

local AGG_VER = GetAddOnMetadata(addonName, "Version")

-- ---------------------------------------------------------------------------
-- Minimal version check (addon messages on local zone channel 1)
-- ---------------------------------------------------------------------------
local VC_PREFIX     = "ATTGOGO"          -- <=16 chars, unique for your addon
local VC_CHANNEL_ID = 1                  -- "1" = General/local zone

C_ChatInfo.RegisterAddonMessagePrefix(VC_PREFIX)

local function VC_ParseVersion(ver)
    local a, b, c = tostring(ver):match("^(%d+)%.(%d+)%.?(%d*)$") -- M.mm.ppp
    return ((tonumber(a) or 0) * 100 + (tonumber(b) or 0)) * 1000 + (tonumber(c) or 0)
end

local function VC_SendMyVersion() C_ChatInfo.SendAddonMessage(VC_PREFIX, "V:" .. AGG_VER, "CHANNEL", tostring(VC_CHANNEL_ID)) end
local VC_HIGHEST = VC_ParseVersion(AGG_VER)

local VC_WARNED_FOR = nil
local vcFrame = CreateFrame("Frame")
vcFrame:RegisterEvent("CHAT_MSG_ADDON")
vcFrame:SetScript("OnEvent", function(_, event, prefix, message)--, channel, sender)
    if event ~= "CHAT_MSG_ADDON" or prefix ~= VC_PREFIX then return end

    local cmd, ver = message:match("^(%u+):(.+)$")
    if cmd ~= "V" or not ver then return end

    local ver_parsed = VC_ParseVersion(ver)
    if ver_parsed > VC_HIGHEST then
        VC_HIGHEST = ver_parsed
        if VC_WARNED_FOR ~= ver_parsed then
            VC_WARNED_FOR = ver_parsed
            print(CTITLE .. "version " .. ver .. " is available, consider upgrading.")
        end
    end
end)

local frame = CreateFrame("Frame")
frame:RegisterEvent("ADDON_LOADED")

local function PrintStartup()
    local version = GetAddOnMetadata(addonName, "Version")
    local author = GetAddOnMetadata(addonName, "Author")
    local coauthor = GetAddOnMetadata(addonName, "X-CoAuthor")
    print(CTITLE .. "v" .. AGG_VER .. ", vibed by: " .. author .. " & " .. coauthor)
end

local function OpenUncollectedForHere()
    local target = Util.ResolvePopupTargetForCurrentContext()
    ShowUncollectedPopup(target)
end

-- Refresh the Uncollected popup to the *current* context, if visible and allowed.
-- @param force boolean (optional) - force rebuild even if current node didn't change
local function RefreshUncollectedPopupForContextIfShown(force)
    local popup = _G.ATTGoGoUncollectedPopup -- gets created in EnsurePopup() on first use
    if not popup:IsShown() then return end

    if not (force or GetSetting("autoRefreshPopupOnZone", true)) then return end

    local target = Util.ResolvePopupTargetForCurrentContext()
    if force or popup.currentData ~= target then
      ShowUncollectedPopup(target)
    end

end

local function ShowMinimapTooltip(tooltip)
    local function AddActionLine(action, text) tooltip:AddLine("|cffaaaaaa" .. action .. "|r: |cffeded44" .. text .. "|r") end
    RequestRaidInfo()
    tooltip:AddLine("|T" .. ICON_MAIN .. ":40:40|t " .. CTITLE .. "v" .. AGG_VER, 1, 1, 1)
    AddActionLine("Left Click", "Open main grid window")
    AddActionLine("Right Click", "Uncollected for current instance/zone")
    AddActionLine("Shift + Left Click", "Options")
    AddActionLine("Drag", "Move icon")
    if GetSetting("DBG_en", false) == true then
        AddActionLine("Alt + Left Click", "Toggle ScriptErrors")
    end
    tooltip:AddLine(" ")
    Tooltip.AddContextProgressTo(tooltip)
end

local function SetupMinimapIcon()
    local ldb = LibStub:GetLibrary("LibDataBroker-1.1", true)
    local icon = LibStub:GetLibrary("LibDBIcon-1.0", true)
    local dataObj = ldb:NewDataObject(addonName, {
        type = "data source",
        text = TITLE,
        icon = ICON_MAIN,
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
        -- Big wave => assume whole-DB refresh; rebuild everything
        DebugLog("BIG wave, cnt = " .. cnt)
        Util.InvalidateProgressCache()
        Util.InvalidateMapProgress()
    else
        -- Small wave => do a context snapshot + popup/active-tab refresh
        local node, info = Util.ResolveContextNode()
        if info.kind == "instance" then
            -- Invalidate the currently relevant difficulty child (and parents)
            local curDiff = ATT.GetCurrentDifficultyID()
            local child = Util.SelectDifficultyChild(node, curDiff) or node
            Util.InvalidateProgressCache(child)
        else
            Util.InvalidateMapProgress(info.uiMapID) -- Zone context: just nuke this map’s memo row
        end

        Util.SaveCurrentContextProgress()
        RefreshUncollectedPopupForContextIfShown(true)
    end
    RefreshActiveTab()
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
    print(CTITLE .. "commands:")
    for _, line in pairs(BuildSlashCommandsText()) do print(line) end
end

local function test()
    if GetSetting("DBG_en", false) ~= true then return end

    local ctx = Util.ResolvePopupTargetForCurrentContext() -- provides instance per-difficulty subset
    local mapID = C_Map.GetBestMapForUnit("player")
    local pkg = ATT.GetCachedDataForMapID(mapID)          -- always provides a combined set
    local node, info = Util.ResolveContextNode()
    print("mapID: " .. mapID)

    local function nt(o, c, t) return ("name=%s; text=%s; %d/%d"):format(tostring(o and o.name or "noname"), tostring(o and o.text or "notext"), (c or 0), (t or 0)) end

    print("ctx: " .. nt(ctx,  Util.ATTGetProgress(ctx)))
    print("pkg: " .. nt(pkg,  Util.ResolveMapProgress(mapID)))
    print("dfc: " .. nt(node, Util.ATTGetProgress(node)))

    if IsInInstance() then
        local name, instType, difficultyID, difficultyName, maxPlayers, dynDifficulty, isDyn, instMapID, grpSize = GetInstanceInfo()
        print(name .. ", " .. instType .. ", difficulty=" .. difficultyID .. " (" .. difficultyName .. "), max players=" .. maxPlayers .. ", instMapID=" .. instMapID .. ", group size=" .. grpSize)
        local mi = ATT.CurrentMapInfo
        print("mapID=" .. mi.mapID .. ", name=" .. mi.name .. ", mapType=" .. mi.mapType .. ", parentMapID=" .. mi.parentMapID)
    end
end

local function Perf(verb)
    if verb == "reset" then
        AGGPerf.reset()
        print(CTITLE .. "Performance data reset")
    else
        AGGPerf.on(verb == "1")
    end
end

local function SetupSlashCmd()
    SlashCmdList["ATTGOGO"] = function(msg)
        local raw = (msg or ""):trim()
        local cmd, rest = raw:match("^(%S+)%s*(.*)$")
        cmd = (cmd or ""):lower()

        local HELP    = { h = true, help = true, ["?"] = true, [""]  = true }
        local ABOUT   = { a = true, about = true }
        local OPTIONS = { o = true, options = true }
        local SHOW    = { s = true, show = true }
        local LIST    = { l = true, list = true }
        local DUMP    = { d = true, dump = true }
        local PERF    = { p = true, perf = true }
        local TEST    = { t = true, test = true }

        if HELP[cmd]    then PrintSlashCmdHelp()        return end
        if ABOUT[cmd]   then AboutUI.Show()             return end
        if OPTIONS[cmd] then OptionsUI.Show()           return end
        if SHOW[cmd]    then ShowMainFrame()            return end
        if LIST[cmd]    then OpenUncollectedForHere()   return end
        if DUMP[cmd]    then DumpCurrentCtx()           return end
        if TEST[cmd]    then test()                     return end
        if PERF[cmd]    then Perf(rest)                 return end
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
    AGGPerf.loadStatsFromDB()
    if GetSetting("DBG_en", false) == true then AGGPerf.on(true) end

    -- === Wait for ATT ("All The Things") ===
    ATT.AddEventHandler("OnReady", function()
        Util.CanonicalizePopupIdFilters()
        SetupMainUI()
        StartGridWarmup()
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
            if IsInInstance() then test() end
            -- slight delay so C_Map / GetInstanceInfo settle
            C_Timer.After(0.15, RefreshUncollectedPopupForContextIfShown)
        end)

        local bossWatcher = CreateFrame("Frame")
        bossWatcher:RegisterEvent("BOSS_KILL")
        bossWatcher:RegisterEvent("ENCOUNTER_END")
        bossWatcher:SetScript("OnEvent", OnThingCollected)

        ATT.AddEventHandler("OnThingCollected", OnThingCollected)

        PrintStartup()
        C_Timer.After(9, VC_SendMyVersion)
    end)
end)
