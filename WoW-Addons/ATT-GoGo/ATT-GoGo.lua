local addonName, addonTable = ...
title = GetAddOnMetadata(addonName, "Title") or "UNKNOWN"

local frame = CreateFrame("Frame")
frame:RegisterEvent("ADDON_LOADED")

local function PrintStartup()
    local version = GetAddOnMetadata(addonName, "Version") or ""
    local author = GetAddOnMetadata(addonName, "Author") or "YePhIcK"
    local coauthor = GetAddOnMetadata(addonName, "X-CoAuthor") or "AI"
    print("|cff00ff00[" .. title .. "]|r " .. version .. " |cffffff00Vibed by:|r " .. author .. " & " .. coauthor)
end

local function OpenUncollectedForCurrentContext()
    local node, info = Util.ResolveContextNode(true)
    if not node then return false end

    -- Always open for instances.
    if info and info.kind == "instance" then
        -- Prefer the current-difficulty child group when available
        ShowUncollectedPopup(Util.SelectDifficultyChild(node, AllTheThings.GetCurrentDifficultyID()))
        return true
    end

    -- For zones: open for the best (top container) zone so we don't churn on sub-zones.
    local mapID = info and info.uiMapID
    local bestZone = ResolveBestZoneNode(mapID)
    if bestZone then
        ShowUncollectedPopup(bestZone)
        return true
    end

    -- No valid ATT zone for this map
    return false
end

-- Refresh the Uncollected popup to the *current* context, if visible and allowed.
local function RefreshUncollectedPopupForContextIfShown()
    if not GetSetting("autoRefreshPopupOnZone", true) then return end

    local popup = _G.ATTGoGoUncollectedPopup
    if not (popup and popup:IsShown()) then return end

    local node, info = Util.ResolveContextNode(true)
    if not node then return end

    if info and info.kind == "instance" then
        -- Only refresh if the instance node actually changed
        local inst = Util.SelectDifficultyChild(node, AllTheThings.GetCurrentDifficultyID())
        if popup.currentData ~= inst then
            ShowUncollectedPopup(inst)
        end
        return
    end

    -- Zones: resolve to the top container zone and refresh only if it changed
    local bestZone = ResolveBestZoneNode(info and info.uiMapID)
    if bestZone and popup.currentData ~= bestZone then
        ShowUncollectedPopup(bestZone)
    end
end

-- Tooltip header helper
local function AddTooltipHeader(tooltip)
    tooltip:AddLine(title, 0, 1, 0)
    tooltip:AddLine("Left-click: Toggle main window", 1, 1, 1)
    tooltip:AddLine("Right-click: Uncollected for current instance/zone", 1, 1, 1)
    tooltip:AddLine("Shift-click: Open options", 1, 1, 1)
    tooltip:AddLine("Drag: Move icon", 1, 1, 1)
end

-- Entry point for tooltip
local function ShowMinimapTooltip(tooltip)
    RequestRaidInfo()
    AddTooltipHeader(tooltip)
    Tooltip.AddContextProgressTo(tooltip)
end

local function SetupMinimapIcon()
    local ldb = LibStub:GetLibrary("LibDataBroker-1.1", true)
    local icon = LibStub:GetLibrary("LibDBIcon-1.0", true)
    if ldb and icon then
        local dataObj = ldb:NewDataObject(addonName, {
            type = "data source",
            text = title,
            icon = "Interface\\AddOns\\ATT-GoGo\\icon-Go2.tga",
            OnClick = function(self, button)
                -- Shift-click opens the Uncollected popup for current instance/zone
                if IsShiftKeyDown() then
                    ShowATTGoGoOptions()
                    return
                end
                if button == "RightButton" then
                    if not OpenUncollectedForCurrentContext() then
                        print("|cffff0000[" .. title .. "]|r Nothing to show for this location.")
                    end
                else
                    if ATTGoGoMainFrame:IsShown() then
                        ATTGoGoMainFrame:Hide()
                    else
                        ShowATTGoGoMain()
                    end
                end
            end,
            OnTooltipShow = ShowMinimapTooltip, -- References Minimap.lua's version
        })
        icon:Register(addonName, dataObj, ATTGoGoDB.minimap)
        if ATTGoGoDB and ATTGoGoDB.minimap and ATTGoGoDB.minimap.hide then
            icon:Hide(addonName)
        else
            icon:Show(addonName)
        end
    else
        print("|cffff0000[" .. title .. "]|r Minimap icon libraries not found! Minimap icon will be unavailable.")
    end
end

local function EnsurePopupIdFilters()
    ATTGoGoCharDB = ATTGoGoCharDB or {}
    ATTGoGoCharDB.popupIdFilters = ATTGoGoCharDB.popupIdFilters or {}
    for k, v in pairs(COLLECTIBLE_ID_FIELDS) do
        if ATTGoGoCharDB.popupIdFilters[k] == nil then
            ATTGoGoCharDB.popupIdFilters[k] = v
        end
    end
end

----------------------------------------------------------------
-- Batch ATT "OnThingCollected" updates (simple version)
----------------------------------------------------------------
local THRESHOLD   = 8
local BATCH_DELAY = 0.40 -- wait this long after the *last* event

local collectedBatch = { count = 0, timer = nil }

local function FlushCollectedBatch()
    local cnt = collectedBatch.count
    collectedBatch.count, collectedBatch.timer = 0, nil

    if cnt >= THRESHOLD then
        -- Big wave => assume whole-DB refresh; rebuild everything
        Util.ClearProgressCache()
        SetupMainUI()           -- full rebuild of main frame widgets (also refreshes data)
        RefreshActiveTab()
    else
        -- Small wave => current-context refresh only
        Util.SaveCurrentContextProgress()
        RefreshActiveTab()
        RefreshUncollectedPopupForContextIfShown()
    end
end

local function OnThingCollected(_node)
    collectedBatch.count = collectedBatch.count + 1

    -- Silence-based debounce: restart a cancelable timer each event
    if collectedBatch.timer then
        collectedBatch.timer:Cancel()
    end
    collectedBatch.timer = C_Timer.NewTimer(BATCH_DELAY, FlushCollectedBatch)
end

frame:SetScript("OnEvent", function(self, event, arg1)
    if arg1 ~= addonName then return end

    -- Ensure DB tables exist
    ATTGoGoDB = ATTGoGoDB or {}
    ATTGoGoCharDB = ATTGoGoCharDB or {}
    ATTGoGoDB.minimap = ATTGoGoDB.minimap or {}

    Debug_Init()

    EnsurePopupIdFilters()

    PrintStartup()

    SetupMinimapIcon()

    -- === Wait for ATT ("All The Things") ===
    local __ATT_INIT_DONE = false
    local function WaitForATT()
        -- Hard dependency check, once.
        if not IsAddOnLoaded("AllTheThings") then
            print("|cffff0000[" .. title .. "]|r All The Things is |cffff2222not loaded|r.")
            print("→ Please enable or install 'All The Things' for " .. title .. " to function.")
            return
        end

        -- Bind once to ATT's lifecycle.
        local ATT_API = _G.AllTheThings or _G.ATTC
        if not (ATT_API and ATT_API.AddEventHandler) then
            print("|cffff0000[" .. title .. "]|r Could not hook ATT events (missing AddEventHandler).")
            return
        end

        ATT_API.AddEventHandler("OnReady", function()
            if __ATT_INIT_DONE then return end
            __ATT_INIT_DONE = true

            ATT = _G.AllTheThings
            print("|cff00ff00[" .. title .. "]|r is ready")
            SetupMainUI()

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

            ATT_API.AddEventHandler("OnThingCollected", OnThingCollected)

            -- keep your progress cache coherent with ATT refreshes
            ATT_API.AddEventHandler("OnInit",         Util.ClearProgressCache)
            ATT_API.AddEventHandler("OnRefresh",      Util.ClearProgressCache)
            ATT_API.AddEventHandler("OnAfterRefresh", Util.ClearProgressCache)
        end)
    end

    WaitForATT()
end)

SLASH_ATTGOGO1 = "/attgogo"
SLASH_ATTGOGO2 = "/gogo"

SlashCmdList["ATTGOGO"] = function(msg)
    local cmd = (msg or ""):lower():trim()

    if cmd == "" or cmd == "?" or cmd == "help" or cmd == "h" then
        print("|cffffff00[" .. title .. " Commands]|r")
        print("/attgogo help        - Show this help")
        print("/attgogo options     - Open the options window")
        print("/attgogo show        - Show the main window")
        print("/attgogo list        - Open Uncollected for current instance/zone")
        print("/attgogo here        - Same as 'list'")
        print("alternatively you can use /gogo")
        return
    end

    if cmd == "options" then ShowATTGoGoOptions() return end
    if cmd == "show"    then ShowATTGoGoMain()    return end

    -- NEW: open Uncollected popup for current context
    if cmd == "list" or cmd == "here" then
        if not OpenUncollectedForCurrentContext() then
            print("|cffff0000[" .. title .. "]|r Nothing to show for this location.")
        end
        return
    end

    print("|cffff0000[" .. title .. "]|r Unknown command. Type '/attgogo help' for options.")
end
