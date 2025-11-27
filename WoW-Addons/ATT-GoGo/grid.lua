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
    if not f.__origBorderColor then f.__origBorderColor = { f:GetBackdropBorderColor() } end

    -- Hover: gold border + hand cursor (only hook once per frame)
    if not f.__hoverHandlersAttached then
        f.__hoverHandlersAttached = true

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
  -- title (created once, reused)
  local title = f.title
  if not title then
    title = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    f.title = title
    title:SetPoint("TOP", 0, -10)
    title:SetJustifyH("CENTER")
    title:SetWordWrap(false)
    title:SetMaxLines(1)
  end
  title:SetWidth(widgetSize - 8)
  title:SetText(Util.NodeDisplayName(data))

  -- optional lockout line (only for instances)
  local lockFS = f.lockFS
  if data.instanceID then
    local isLocked, _, _, lockoutIndex = IsInstanceLockedOut(data)
    if isLocked then
      if not lockFS then
        lockFS = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        f.lockFS = lockFS
        lockFS:SetPoint("TOP", f.title, "BOTTOM", 0, -2)
        lockFS:SetJustifyH("CENTER")
      end
      lockFS:SetWidth(widgetSize - 8)
      local reset = select(3, GetSavedInstanceInfo(lockoutIndex))
      lockFS:SetText("|cffffd200" .. Util.FormatTime(reset) .. "|r")
      lockFS:Show()
    elseif lockFS then
      lockFS:Hide()
    end
  elseif lockFS then
    lockFS:Hide() -- zone widget or instance without lockout: hide any previous lock text
  end

  -- stats line
  local statsFS = f.statsFS
  if not statsFS then
    statsFS = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    f.statsFS = statsFS
    statsFS:SetPoint("BOTTOM", 0, 8)
    statsFS:SetJustifyH("CENTER")
  end
  statsFS:SetWidth(widgetSize - 8)
  statsFS:SetText(("%d / %d (%.1f%%)"):format(collected, total, percent))
end

-- ownerNode is the node that carries mapID/instanceID for DB lookups (e.g., the instance node)
function Tile.SetProgressWidgetTooltip(f, data, collected, total, percent, isZone, ownerNode)
  Tooltip.CreateTooltip(f, "ANCHOR_RIGHT", function()
    Tooltip.AddLine(Util.NodeDisplayName(data))
    Tooltip.AddProgress(GameTooltip, data, collected, total, percent, isZone, ownerNode)
  end)
end

-- list of difficulties is obtained by running `/run for i=1,300 do local n=GetDifficultyInfo(i); if n then print(i, n) end end`
local DIFF_LABEL = {
  [1]   = "5",  [2]   = "5H", [8]   = "CM",  [11]  = "3H",  [12]  = "3",
  [3]   = "10", [4]   = "25", [5]   = "10H", [6]   = "25H", [7]   = "LFR", [9]   = "40", [14]  = "Flex", [148] = "20",
  [173] = "5",  [174] = "5H", [175] = "10", [176] = "25", [193] = "10H", [194] = "25H",
  [237] = "Celestial",
  -- extras (unconfirmed)
  [7]   = "LFR",    [15]  = "Flex/H", [16]  = "Flex/M",  [17]  = "LFR",     [102] = "LFG",
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

  if #diffs == 0 then
    if parentFrame.infoBtn then
      parentFrame.infoBtn:Hide()
    end
    return
  end

  local btn = parentFrame.infoBtn
  if not btn then
    btn = CreateFrame("Button", nil, parentFrame)
    btn:SetSize(16, 16)
    btn:SetPoint("TOPRIGHT", parentFrame, "TOPRIGHT", -6, -6)

    local tex = btn:CreateTexture(nil, "ARTWORK")
    tex:SetAllPoints(btn)
    tex:SetTexture("Interface\\FriendsFrame\\InformationIcon")
    btn.iconTex = tex

    Tooltip.CreateTooltip(btn, "ANCHOR_LEFT", function()
      GameTooltip:AddLine("Difficulties", 1, 1, 1)
      local list = btn.diffs
      if not list then return end
      table.sort(list, function(a, b) return a.d < b.d end)
      for _, r in ipairs(list) do
        local p   = (r.t > 0) and (r.c / r.t * 100) or 0
        local tag = DIFF_LABEL[r.d] or r.d
        GameTooltip:AddLine(("• %s — %d/%d (%.1f%%)"):format(tag, r.c, r.t, p), 0.9, 0.9, 0.9)
      end
    end)

    parentFrame.infoBtn = btn
  end

  btn.diffs = diffs
  btn:Show()
end

function Tile.SetupFavoriteStar(f, data, isZone, onFavToggled)
  local favKey = Util.FavKey(data, isZone)

  if not f.starBtn then
    local starBtn = CreateFrame("Button", nil, f)
    starBtn:SetSize(16, 16)
    starBtn:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", -6, 6)
    starBtn:SetFrameLevel(f:GetFrameLevel() + 10)

    local ICON_FAV = "Interface\\COMMON\\ReputationStar"
    starBtn:SetNormalTexture(ICON_FAV)

    f.starBtn = starBtn
  end

  local starBtn = f.starBtn

  local function paintStar()
    local UV_ON  = {0,   0.5, 0,   0.5}  -- filled
    local UV_OFF = {0.5, 1.0, 0,   0.5}  -- hollow
    local on     = Util.IsFavoriteKey(favKey)
    local normal = starBtn:GetNormalTexture()
    if normal then
      normal:SetAllPoints()
      normal:SetTexCoord(unpack(on and UV_ON or UV_OFF))
      normal:SetAlpha(on and 1 or 0.8)
    end
  end
  paintStar()

  starBtn:SetScript("OnClick", function()
    Util.ToggleFavoriteKey(favKey)
    paintStar()
    if onFavToggled then onFavToggled() end
  end)
end

-- Main: Create a progress widget for grid
function Tile.CreateProgressWidget(existing, content, data, x, y, widgetSize, padding, isZone, attNode, onFavToggled)
return AGGPerf.wrap("Tile.CreateProgressWidget", function() -- 214    0.629    1.148    1.669    0.297  134.556  Tile.CreateProgressWidget
    local f = existing
    if not f then
      f = CreateFrame("Frame", nil, content, BackdropTemplateMixin and "BackdropTemplate" or nil)
      f:SetBackdrop({
        bgFile   = "Interface\\ChatFrame\\ChatFrameBackground",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile     = true, tileSize = 16, edgeSize = 20,
        insets   = { left = 3, right = 3, top = 3, bottom = 3 },
      })
    else
      f:SetParent(content)
    end

    -- size + layout for this grid cell
    f:SetSize(widgetSize, 60)
    f:ClearAllPoints()
    f:SetPoint("TOPLEFT", x * (widgetSize + padding), -y * (60 + padding))

    -- instance/zone icon in top-left (same toggle), created once and reused
    if GetSetting("showInstanceIconOnWidgets", true) then
      if not f.icon then
        local tex = f:CreateTexture(nil, "ARTWORK")
        tex:SetSize(48, 48)
        tex:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
        f.icon = tex
      end
      Util.ApplyNodeIcon(f.icon, attNode or data, { texCoord = { 0.07, 0.93, 0.07, 0.93 } })
      f.icon:Show()
    elseif f.icon then
      f.icon:Hide()
    end

    local collected, total, percent
    if isZone then
      collected, total, percent = Util.ResolveMapProgress(data.mapID)
      Util.SaveZoneProgressByMapID(data.mapID)
    else
      local node = attNode or data
      collected, total, percent = Util.ATTGetProgress(node)
      if node and node.instanceID then Util.SaveInstanceProgressByNode(node) end -- only persist for real instance nodes
    end

    Tile.SetProgressWidgetVisuals(f, data, percent, isZone)
    Tile.AddProgressWidgetText(f, data, widgetSize, collected, total, percent, attNode)

    -- N.B.: pass an owner with mapID/instanceID so "other toons" can be shown
    local owner = isZone and { mapID = data.mapID } or (attNode or data)
    Tile.SetProgressWidgetTooltip(f, data, collected, total, percent, isZone, owner)

    -- click opens the ATT map package for zones, or the instance/era node otherwise
    Tile.AttachClickAndHoverUX(f, attNode or data)

    if attNode.instanceID then
      AttachInfoIcon(f, attNode)
    elseif f.infoBtn then
      f.infoBtn:Hide()
    end

    -- bottom-right favorite toggle using Blizzard's Reputation star (2x2 atlas)
    Tile.SetupFavoriteStar(f, attNode or data, isZone, onFavToggled)

    f:Show()
    return f
end)
end


local mainFrame = CreateFrame("Frame", "ATTGoGoMainFrame", UIParent, "BasicFrameTemplateWithInset")

local expTabY = -25
local zoneTabY = -45
local summaryY = -70  -- Below both rows of tabs

-- Background pre-warm of grids some time after the UI is first shown
local gridWarmupScheduled = false
local gridWarmupQueue = {}
local gridWarmupTicker = nil

local function BuildGridWarmupQueue()
    wipe(gridWarmupQueue)
    -- Use tabOrder so we process tabs in a deterministic order
    for idx = 1, #tabOrder do
        local tabId = tabOrder[idx]
        local tab = tabButtons[tabId]
        local content = tab and tab.content
        local scroll = content and content.scroll
        if scroll and scroll.Refresh then
            gridWarmupQueue[#gridWarmupQueue + 1] = scroll
        end
    end
end

function StartGridWarmup()
    if gridWarmupScheduled then return end -- only schedule once
    gridWarmupScheduled = true

    -- start 5 seconds later to avoid impacting initial UI responsiveness
    C_Timer.After(5, function()
        BuildGridWarmupQueue()
        if #gridWarmupQueue == 0 then return end

        local index = 1
        gridWarmupTicker = C_Timer.NewTicker(0.05, function()
            local scroll = gridWarmupQueue[index]
            index = index + 1

            if scroll and scroll.Refresh then scroll:Refresh() end

            if index > #gridWarmupQueue then
                if gridWarmupTicker then
                    gridWarmupTicker:Cancel()
                    gridWarmupTicker = nil
                    print(CTITLE .. "Grid warmup complete")
                end
            end
        end)
    end)
end

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

    -- NOTE: first arg is the *existing* frame (or nil) for pooling
    local tileFactory = function(existing, content, data, x, y, widgetSize, padding)
      local attNode = isZone and Util.GetMapRoot(data.mapID) or (data.attNode or data)
      return Tile.CreateProgressWidget(existing, content, data, x, y, widgetSize, padding, isZone, attNode, ResortAndRefresh)
    end

    local scroll = gridFunc(tabContent, entries, tileFactory, 160, 10)
    -- expose direct refresh on the tab content so callers don't have to hunt children
    tabContent.scroll  = scroll
    tabContent.Refresh = scroll.Refresh

    tabButtons[tabId].content = tabContent
end

function Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, tabs, contentY, isZone, filterFunc, sortFunc, gridFunc)
    for i, t in ipairs(tabs) do
        local tabId = t.id  -- use the exact id used when creating the tab
        local entries = PrepareTabData(t, isZone, filterFunc, sortFunc)

        -- tiles will compute progress on-demand via Tile.CreateProgressWidget
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
  local includeRemoved = GetSetting("includeRemoved", false)
  local frameWidth     = scroll:GetWidth()
  local cols           = Util.GetGridCols(frameWidth, widgetSize, padding)
  local x, y           = 0, 0
  local visibleCount   = 0

  local perf = AGGPerf.auto("Grid.Populate:tileFactory")
  for _, entry in ipairs(dataset) do
    if includeRemoved or (not entry.removed) then
      visibleCount = visibleCount + 1

      local f = widgets[visibleCount]
      if f then
        -- reuse existing widget frame
        f = tileFactory(f, content, entry, x, y, widgetSize, padding)
      else
        -- create a new widget frame and add it to the pool
        f = tileFactory(nil, content, entry, x, y, widgetSize, padding)
        widgets[visibleCount] = f
      end
      if f then f:Show() end

      x = x + 1
      if x >= cols then x = 0; y = y + 1 end
    end
  end
  perf()

  -- hide any now-unused pooled widgets
  for i = visibleCount + 1, #widgets do
    local f = widgets[i]
    if f then f:Hide() end
  end

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
    -- Zone tabs: no eager ResolveMapProgress; tiles compute progress on demand
    Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, zones, -105, true, nil, nil, Grid.Create)

    Tabs.InitialTabSelection(mainFrame, tabOrder, SelectTab)
    RegisterMainFrameEvents()
done()
end
