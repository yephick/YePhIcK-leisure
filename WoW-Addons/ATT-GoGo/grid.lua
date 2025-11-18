local expansions = nil
local tabButtons = {}
local currentTab = nil
local tabOrder = {}  -- tab ID lookup

local Tabs = {}
local Summary = {}
local Grid = {}
local Tile = {}

-- Whole-widget click + hover border + hand cursor
function Tile.AttachClickAndHoverUX(f, data)
    -- Click anywhere on the widget to open the popup
    f:EnableMouse(true)
    f:SetScript("OnMouseUp", function(self, button)
        if button == "LeftButton" or button == "MiddleButton" then
            ShowUncollectedPopup(data)
        end
    end)
    f.__origBorderColor = { f:GetBackdropBorderColor() }

    -- Hover: gold border + hand cursor
    f:HookScript("OnEnter", function(self)
        self:SetBackdropBorderColor(1, 0.82, 0, 1)   -- gold-ish
        SetCursor("Interface\\CURSOR\\Point")
    end)

    f:HookScript("OnLeave", function(self)
        self:SetBackdropBorderColor( self.__origBorderColor[1], self.__origBorderColor[2],
                                     self.__origBorderColor[3], self.__origBorderColor[4] )
        ResetCursor()
    end)
end

function Tile.SetProgressWidgetVisuals(f, data, percent, isZone)
  local r, g, b = GetCompletionColor(percent)
  f:SetBackdropColor(r, g, b, 0.85)
  local br, bg, bb = math.min(r * 2.2, 1), math.min(g * 2.2, 1), math.min(b * 2.2, 1)
  f:SetBackdropBorderColor(br, bg, bb, 1)
  f:SetAlpha(1)
  if not isZone then
    local isLocked, numDown, numBosses = IsInstanceLockedOut(data)
    if isLocked then
      if numBosses == 0 or numDown >= numBosses then
        f:SetBackdropColor(0.25, 0.25, 0.25, 0.35)
        f:SetBackdropBorderColor(0.22, 0.22, 0.22, 0.70)
        f:SetAlpha(0.40)
      else
        f:SetBackdropBorderColor(0.8, 0.85, 0.93, 1)
      end
    end
  end
end

function Tile.AddProgressWidgetText(f, data, widgetSize, collected, total, percent, attNode)
  local title = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
  title:SetPoint("TOP", 0, -10)
  title:SetJustifyH("CENTER")
  title:SetWidth(widgetSize - 8)
  title:SetText(Util.NodeDisplayName(data))
  title:SetWordWrap(false)
  title:SetMaxLines(1)
  if data.instanceID  then
    local isLocked, _, _, lockoutIndex = IsInstanceLockedOut(data)
    if isLocked then
      local reset = select(3, GetSavedInstanceInfo(lockoutIndex))
      local lockFS = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
      lockFS:SetPoint("TOP", title, "BOTTOM", 0, -2)
      lockFS:SetJustifyH("CENTER")
      lockFS:SetWidth(widgetSize - 8)
      lockFS:SetText("|cffffd200" .. Util.FormatTime(reset) .. "|r")
    end
  end
  local stats = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
  stats:SetPoint("BOTTOM", 0, 8)
  stats:SetJustifyH("CENTER")
  stats:SetWidth(widgetSize - 8)
  stats:SetText(("%d / %d (%.1f%%)"):format(collected, total, percent))
end

-- ownerNode is the node that carries mapID/instanceID for DB lookups (e.g., the instance node)
function Tile.SetProgressWidgetTooltip(f, data, collected, total, percent, isZone, ownerNode)
  Tooltip.CreateTooltip(f, "ANCHOR_RIGHT", function()
    Tooltip.AddLine(Util.NodeDisplayName(data))
    Tooltip.AddProgress(GameTooltip, data, collected, total, percent, isZone, ownerNode)
  end)
end

local DIFF_LABEL = {
  [1] = "5N", [2] = "5H", [8] = "CM",
  [3] = "10N", [4] = "25N", [5] = "10H", [6] = "25H", [9] = "40",
  [7] = "LFR", [14] = "Flex/N", [15] = "Flex/H", [16] = "M",
  [114] = "DS LFR", [115] = "DS LFR", [118] = "SoD LFR", [119] = "SoD LFR", [120] = "SoD LFR", [121] = "SoD LFR",
}

local function AttachInfoIcon(parentFrame, eraNode)
  -- collect per-difficulty rows present in this era wrapper
  local diffs = {}
  for _, ch in ipairs(eraNode.g) do
      local d = ch.difficultyID
      if d then
          local c, t = Util.ATTGetProgress(ch)
          diffs[#diffs+1] = { d = d, c = c, t = t }
      end
  end
  if #diffs == 0 then return end

  local btn = CreateFrame("Button", nil, parentFrame)
  btn:SetSize(16, 16)
  btn:SetPoint("TOPRIGHT", parentFrame, "TOPRIGHT", -6, -6)

  local tex = btn:CreateTexture(nil, "ARTWORK")
  tex:SetAllPoints(btn)
  tex:SetTexture("Interface\\FriendsFrame\\InformationIcon")

  Tooltip.CreateTooltip(btn, "ANCHOR_LEFT", function()
    GameTooltip:AddLine("Difficulties", 1, 1, 1)
    table.sort(diffs, function(a,b) return a.d < b.d end)
    for _, r in ipairs(diffs) do
      local p = (r.t > 0) and (r.c / r.t * 100) or 0
      local tag = DIFF_LABEL[r.d] or r.d
      GameTooltip:AddLine(("• %s — %d/%d (%.1f%%)"):format(tag, r.c, r.t, p), 0.9, 0.9, 0.9)
    end
  end)
end

-- Main: Create a progress widget for grid
function Tile.CreateProgressWidget(content, data, x, y, widgetSize, padding, isZone, attNode, onFavToggled)
return AGGPerf.wrap("Tile.CreateProgressWidget", function() -- 214    0.629    1.148    1.669    0.297  134.556  Tile.CreateProgressWidget
    local f = CreateFrame("Frame", nil, content, BackdropTemplateMixin and "BackdropTemplate" or nil)
    f:SetSize(widgetSize, 60)
    f:SetPoint("TOPLEFT", x * (widgetSize + padding), -y * (60 + padding))

    f:SetBackdrop({
        bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 20,
        insets = { left = 3, right = 3, top = 3, bottom = 3 }
    })

    -- Instance/Zone icon in top-left (same toggle)
    if GetSetting("showInstanceIconOnWidgets", true) then
        local tex = f:CreateTexture(nil, "ARTWORK")
        tex:SetSize(48, 48)
        tex:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
        Util.ApplyNodeIcon(tex, attNode or data, { texCoord = { 0.07, 0.93, 0.07, 0.93 } })
    end

    local perf = AGGPerf.auto("Tile.CreateProgressWidget:calc_progress") -- 214    0.008    0.020    0.026    0.004    1.779  Tile.CreateProgressWidget:calc_progress
    local collected, total, percent
    if isZone then
      collected, total, percent = Util.ResolveMapProgress(data.mapID)
    else
      collected, total, percent = Util.ATTGetProgress(attNode or data)
    end
    perf()
    Tile.SetProgressWidgetVisuals(f, data, percent, isZone)
    Tile.AddProgressWidgetText(f, data, widgetSize, collected, total, percent, attNode)
    -- N.B.: pass an owner with mapID/instanceID so "other toons" can be shown
    local owner = isZone and { mapID = data.mapID } or (attNode or data)
    Tile.SetProgressWidgetTooltip(f, data, collected, total, percent, isZone, owner)
    -- click opens the ATT map package for zones, or the instance/era node otherwise
    Tile.AttachClickAndHoverUX(f, attNode or data)
    if attNode.instanceID then
      AttachInfoIcon(f, attNode)
    end

    -- bottom-right favorite toggle using Blizzard's Reputation star (2x2 atlas)
    local favKey  = Util.FavKey(attNode or data, isZone)

    local starBtn = CreateFrame("Button", nil, f)
    starBtn:SetSize(16, 16)
    starBtn:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", -6, 6)
    starBtn:SetFrameLevel(f:GetFrameLevel() + 10)

    local ICON_FAV = "Interface\\COMMON\\ReputationStar"
    starBtn:SetNormalTexture(ICON_FAV)

    local function paintStar()
      local UV_ON  = {0,   0.5, 0,   0.5}  -- filled
      local UV_OFF = {0.5, 1.0, 0,   0.5}  -- hollow
      local on = Util.IsFavoriteKey(favKey)
      local normal = starBtn:GetNormalTexture();
      normal:SetAllPoints()
      normal:SetTexCoord(unpack(on and UV_ON or UV_OFF))
      normal:SetAlpha(on and 1 or 0.8)
    end
    paintStar()

    starBtn:SetScript("OnClick", function()
      Util.ToggleFavoriteKey(favKey)
      paintStar()
      onFavToggled()
    end)

    return f
end)
end


local mainFrame = CreateFrame("Frame", "ATTGoGoMainFrame", UIParent, "BasicFrameTemplateWithInset")

local expTabY = -25
local zoneTabY = -45
local summaryY = -70  -- Below both rows of tabs

function Tabs.CreateTabs(mainFrame, tabButtons, tabOrder, tabs, yOffset, isZone, startIndex, SelectTab)
    local lastTab = nil
    for i, t in ipairs(tabs) do
        local tabIndex = (startIndex or 1) + (i - 1)
        local tabName = "ATTGoGoMainFrameTab" .. tabIndex
        local tab = CreateFrame("Button", tabName, mainFrame, "OptionsFrameTabButtonTemplate")
        tab:SetFrameStrata(mainFrame:GetFrameStrata())
        tab:SetFrameLevel(mainFrame:GetFrameLevel() + 10)  -- keep tabs above the frame art every time
        tab:SetText(t.name)
        if isZone then
            tab.zoneData = t.node
        end
        tab:SetScript("OnClick", function(self)
            PanelTemplates_SetTab(mainFrame, tabIndex)
            for _, btn in pairs(tabButtons) do
                btn.content:Hide()
            end
            SelectTab(tabOrder[tabIndex])
        end)
        if i == 1 then
            tab:SetPoint("TOPLEFT", mainFrame, "TOPLEFT", 10, yOffset)
        else
            tab:SetPoint("LEFT", lastTab, "RIGHT", -15, 0)
        end
        PanelTemplates_TabResize(tab, 0)
        tabButtons[t.id] = tab
        tabOrder[tabIndex] = t.id
        lastTab = tab
    end
end

-- Prepare tab data based on type
local function PrepareTabData(t, isZone, filterFunc, sortFunc)
    local entries = {}
    if isZone then
        for i, child in pairs(t.node.g or {}) do
            local mid = child and child.mapID
            if mid then
                local entry = {
                    mapID   = mid,
                    name    = child.text or child.name,
                    removed = Util.IsNodeRemoved(child),
                }
                if (not filterFunc) or filterFunc(entry) then entries[#entries+1] = entry end
            end
        end
    else
        entries = GetInstancesForExpansion(t.id)
    end
    if sortFunc then
        table.sort(entries, sortFunc)
    end
    return entries
end

-- Create UI content for a tab
local function CreateTabContentUI(mainFrame, tabId, entries, contentY, isZone, gridFunc, sortFunc)
    local tabContent = CreateFrame("Frame", nil, mainFrame)
    tabContent:SetPoint("TOPLEFT", 5, contentY)
    tabContent:SetPoint("BOTTOMRIGHT", -5, 5)
    tabContent:Hide()

    -- Favorites-first comparator (then fall back to caller sort, else name)
    local function FavFirstSort(a, b)
      local fa = Util.IsFavoriteKey(Util.FavKey(a, isZone))
      local fb = Util.IsFavoriteKey(Util.FavKey(b, isZone))
      if fa ~= fb then return fa end
      if sortFunc then return sortFunc(a, b) end
      return (a.name or "") < (b.name or "")
    end

    -- Initial favorites-first ordering
    table.sort(entries, FavFirstSort)

    -- Helper used by the star click to re-sort + refresh
    local function ResortAndRefresh()
      table.sort(entries, FavFirstSort)
      tabContent.scroll:Refresh()
    end

    local tileFactory = function(content, data, x, y, widgetSize, padding)
      local attNode = isZone and Util.GetMapRoot(data.mapID) or (data.attNode or data)
      return Tile.CreateProgressWidget(content, data, x, y, widgetSize, padding, isZone, attNode, ResortAndRefresh)
    end

    local scroll = gridFunc(tabContent, entries, tileFactory, 160, 10)
    -- expose direct refresh on the tab content so callers don't have to hunt children
    tabContent.scroll  = scroll
    tabContent.Refresh = scroll.Refresh

    tabButtons[tabId].content = tabContent
end

function Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, tabs, contentY, isZone, filterFunc, sortFunc, gridFunc)
    -- Zones: snapshot each zone entry that we present as a widget
    -- Instances: snapshot each instance entry that has an instanceID
    local saveFn = isZone and Util.SaveZoneProgressByMapID
                           or Util.SaveInstanceProgressByNode

    for i, t in ipairs(tabs) do
        local tabId = t.id  -- use the exact id used when creating the tab
        local entries = PrepareTabData(t, isZone, filterFunc, sortFunc)

        for _, e in ipairs(entries) do
            if isZone then
                saveFn(e.mapID)
            else
                saveFn(e.attNode)
            end
        end

        CreateTabContentUI(mainFrame, tabId, entries, contentY, isZone, gridFunc, sortFunc)
    end
end

function Tabs.ZoneEntrySort(a, b)
    local ac, at = Util.ResolveMapProgress(a.mapID)
    local bc, bt = Util.ResolveMapProgress(b.mapID)
    local aCompleted = (at > 0 and ac >= at)
    local bCompleted = (bt > 0 and bc >= bt)

    if aCompleted ~= bCompleted then
        return not aCompleted
    end

    return a.name:lower() < b.name:lower()
end

-- resolve saved/last tab index or fall back to 1
local function ResolveSavedTabIndex(tabOrder)
    local saved = GetCharSetting("lastTabID", nil)
    if saved then
        for i, id in ipairs(tabOrder) do
            if id == saved then return i end
        end
    end
    return 1
end

function Tabs.InitialTabSelection(mainFrame, tabOrder, SelectTab)
    PanelTemplates_SetNumTabs(mainFrame, #tabOrder)
    local idxToSelect = ResolveSavedTabIndex(tabOrder)
    PanelTemplates_SetTab(mainFrame, idxToSelect)
    SelectTab(tabOrder[idxToSelect])
end

-- Create summary bar (background + text)
function Summary.Create(mainFrame, summaryY)
    local inset = mainFrame.Inset or mainFrame
    mainFrame.summaryBg = inset:CreateTexture(nil, "ARTWORK")
    mainFrame.summaryText = inset:CreateFontString(nil, "OVERLAY", "GameFontNormalHuge")

    -- Expansion summary background
    mainFrame.summaryBg:ClearAllPoints()
    mainFrame.summaryBg:SetPoint("TOPLEFT", mainFrame, "TOPLEFT", 5, summaryY)
    mainFrame.summaryBg:SetPoint("TOPRIGHT", mainFrame, "TOPRIGHT", -5, summaryY)
    mainFrame.summaryBg:SetHeight(38)
    mainFrame.summaryBg:SetColorTexture(0.15, 0.15, 0.15, 0.7)

    -- Expansion summary text
    mainFrame.summaryText:SetPoint("CENTER", mainFrame.summaryBg, "CENTER", 0, 0)
    mainFrame.summaryText:SetJustifyH("CENTER")
    mainFrame.summaryText:SetText("Collected: 0 / 0 (0.00%)")
end

-- Set summary bar text and color
function Summary.Update(mainFrame, collected, total)
    local percent = total > 0 and (collected / total * 100) or 0
    mainFrame.summaryText:SetFormattedText("Collected: %d / %d (%.2f%%)", collected, total, percent)
    local r, g, b = GetCompletionColor(percent)
    mainFrame.summaryBg:SetColorTexture(r, g, b, 0.34)
end

-- For expansion tabs
function Summary.UpdateExpansion(mainFrame, expID)
    local instances = GetInstancesForExpansion(expID)
    local c, t = Util.GetCollectionProgress(instances)
    Summary.Update(mainFrame, c, t)
end

-- For zone tabs
function Summary.UpdateZone(mainFrame, tab, tabButtons)
    local collected, total = Util.ATTGetProgress(tab.zoneData)
    Summary.Update(mainFrame, collected, total)
end

-- Helper: Populate a frame with widgets in a grid
function Grid.Populate(content, dataset, tileFactory, widgets, widgetSize, padding, scroll)
local done = AGGPerf.auto("Grid.Populate")
  Util.ClearChildrenOrTabs(content)
  wipe(widgets)

  local includeRemoved = GetSetting("includeRemoved", false)
  local frameWidth = scroll:GetWidth()
  local cols = Util.GetGridCols(frameWidth, widgetSize, padding)
  local x, y = 0, 0

  local perf = AGGPerf.auto("Grid.Populate:tileFactory")
  for _, entry in ipairs(dataset) do
    if includeRemoved or (not entry.removed) then
      local f = tileFactory(content, entry, x, y, widgetSize, padding)
      widgets[#widgets+1] = f
      x = x + 1
      if x >= cols then x = 0; y = y + 1 end
    end
  end
  perf()
  content:SetSize(frameWidth, (y + 1) * (60 + padding) + 80)
done()
end

-- Factory: Create a scrolling grid for any dataset and widget factory
function Grid.Create(parent, dataset, tileFactory, widgetSize, padding)
    local scroll = CreateFrame("ScrollFrame", nil, parent, "UIPanelScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", 10, -10)
    scroll:SetPoint("BOTTOMRIGHT", -30, 10)

    local content = CreateFrame("Frame", nil, scroll)
    content:SetSize(1, 1)
    scroll:SetScrollChild(content)

    local widgets = {}

    local Debounced = Util.Debounce(function() Grid.Populate(content, dataset, tileFactory, widgets, widgetSize, padding, scroll) end, 0.08)

    scroll:SetScript("OnShow", Debounced)
    parent:HookScript("OnSizeChanged", Debounced)

    scroll.Refresh = Debounced
    return scroll
end

local function CreateMainFrame()
    local f = mainFrame
    f:SetSize(724, 612)

    f:SetClampedToScreen(true)
    f:SetFrameStrata("MEDIUM")
    f:SetFrameLevel(10)
    Util.EnableDragPersist(f, "mainWindowPos")
    f:SetResizable(true)

    if f.SetResizeBounds then f:SetResizeBounds(700, 360, 1600, 1200) end

    Util.AddResizerCorner(f, "mainWindowPos", function() end)
    Util.PersistOnSizeChanged(f, "mainWindowPos", function() end)

    f:Hide()
    f.TitleText:SetText(TITLE .. " - Progress summaries")
end

-- Helper: Create gear icon button for options
local function CreateOptionsButton()
    local optionsBtn = CreateFrame("Button", nil, mainFrame)
    optionsBtn:SetSize(16, 16)
    optionsBtn:SetPoint("TOPRIGHT", mainFrame, "TOPRIGHT", -30, -3)
    optionsBtn:SetNormalTexture("Interface\\Icons\\INV_Misc_Gear_01")
    optionsBtn:SetHighlightTexture("Interface\\Icons\\INV_Misc_Gear_01", "ADD")
    Util.SetTooltip(optionsBtn, "ANCHOR_LEFT", "Options", "Open " .. TITLE .. " options")
    optionsBtn:SetScript("OnClick", function() OptionsUI.Show() end)
end


local function SelectTab(tabID)
    if currentTab then currentTab:Hide() end
    SetCharSetting("lastTabID", tabID)
    local tab = tabButtons[tabID]
    tab.content:Show()
    currentTab = tab.content
    -- Update summary for expansions
    if type(tabID) == "number" then
        Summary.UpdateExpansion(mainFrame, tabID)
    else
        Summary.UpdateZone(mainFrame, tab, tabButtons)
    end
end

function ShowMainFrame()
    Util.LoadFramePosition(mainFrame, "mainWindowPos", "TOP", -36, -48)
    mainFrame:Show()
    for _, btn in pairs(tabButtons) do
        btn.content:Hide()
    end
    -- pick saved tab if available, else first
    local idxToSelect = ResolveSavedTabIndex(tabOrder)
    PanelTemplates_SetTab(mainFrame, idxToSelect)
    SelectTab(tabOrder[idxToSelect])
end

-- Helper: Register mainFrame events for updates/redraws
local function RegisterMainFrameEvents()
    mainFrame:RegisterEvent("UPDATE_INSTANCE_INFO")
    mainFrame:RegisterEvent("PLAYER_ENTERING_WORLD")
    mainFrame:SetScript("OnEvent", RefreshActiveTab)
end

-- refresh the currently-visible grid
function RefreshActiveTab()
    if currentTab and currentTab:IsShown() then currentTab:Refresh() end
end

function SetupMainUI()
local done = AGGPerf.auto("SetupMainUI")
    RequestRaidInfo()
    Util.ClearATTSearchCache()

    CreateMainFrame()
    CreateOptionsButton()
    table.insert(UISpecialFrames, "ATTGoGoMainFrame") -- Escape closes window
    Summary.Create(mainFrame, summaryY)

    expansions = BuildExpansionList() -- dynamically built 1-based list
    local zones = BuildZoneList()

    Util.ClearChildrenOrTabs(tabButtons) -- Remove all old tabs/buttons/content from previous runs (in case of reload)
    Tabs.CreateTabs(mainFrame, tabButtons, tabOrder, expansions, expTabY, false, 1, SelectTab)
    Tabs.CreateTabs(mainFrame, tabButtons, tabOrder, zones, zoneTabY, true, #expansions + 1, SelectTab)

    Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, expansions, summaryY - 35, false, nil, nil, Grid.Create)
    Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, zones, -105, true,
        function(entry) local done = AGGPerf.auto("tab factory Util.ResolveMapProgress"); local _, t = Util.ResolveMapProgress(entry.mapID); done(); return (t or 0) > 0 end,
        Tabs.ZoneEntrySort, Grid.Create)

    Tabs.InitialTabSelection(mainFrame, tabOrder, SelectTab)
    RegisterMainFrameEvents()
done()
end
