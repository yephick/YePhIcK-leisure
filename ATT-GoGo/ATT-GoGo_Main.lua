local expansions = nil
local tabButtons = {}
local currentTab = nil

Tabs = {}
Summary = {}
Grid = {}

local mainFrame = CreateFrame("Frame", "ATTGoGoMainFrame", UIParent, "BasicFrameTemplateWithInset")

--local bookmarksFrame
--local bookmarkButtons = {}

local expTabY = -25
local zoneTabY = -45
local summaryY = -70  -- Below both rows of tabs


function Tabs.CreateTabs(mainFrame, tabButtons, tabOrder, tabs, yOffset, isZone, startIndex, SelectTab)
    local lastTab = nil
    for i, t in ipairs(tabs) do
        local tabIndex = (startIndex or 1) + (i - 1)
        local tabName = "ATTGoGoMainFrameTab" .. tabIndex
        local tab = CreateFrame("Button", tabName, mainFrame, "OptionsFrameTabButtonTemplate")
        tab:SetText(t.name)
        if isZone then
            tab.zoneData = t.node
        end
        tab:SetScript("OnClick", function(self)
            PanelTemplates_SetTab(mainFrame, tabIndex)
            for _, btn in pairs(tabButtons) do
                if btn.content then btn.content:Hide() end
            end
            if SelectTab then SelectTab(tabOrder[tabIndex]) end
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
        for _, child in ipairs(t.node.g or {}) do
            if not filterFunc or filterFunc(child) then
                table.insert(entries, child)
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
local function CreateTabContentUI(mainFrame, tabId, entries, contentY, isZone, gridFunc)
    local tabContent = CreateFrame("Frame", nil, mainFrame)
    tabContent:SetPoint("TOPLEFT", 5, contentY)
    tabContent:SetPoint("BOTTOMRIGHT", -5, 5)
    tabContent:Hide()

    local widgetFactory = function(content, data, x, y, widgetSize, padding)
        local attNode = data.attNode or data
        return Widget.CreateProgressWidget(content, data, x, y, widgetSize, padding, isZone, attNode)
    end
    gridFunc(tabContent, entries, widgetFactory, 160, 10)
    tabButtons[tabId].content = tabContent
end

function Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, tabs, contentY, isZone, filterFunc, sortFunc, gridFunc)
    for i, t in ipairs(tabs) do
        local tabId = t.id  -- use the exact id used when creating the tab
        local entries = PrepareTabData(t, isZone, filterFunc, sortFunc)
        CreateTabContentUI(mainFrame, tabId, entries, contentY, isZone, gridFunc)
    end
end

function Tabs.ZoneEntrySort(a, b)
    local aIsCity = a.isCity == true
    local bIsCity = b.isCity == true

    local aCompleted = (tonumber(a.progress or a.collected or 0) >= tonumber(a.total or 0) and tonumber(a.total or 0) > 0)
    local bCompleted = (tonumber(b.progress or b.collected or 0) >= tonumber(b.total or 0) and tonumber(b.total or 0) > 0)

    if aCompleted ~= bCompleted then
        return not aCompleted
    end

    if aIsCity ~= bIsCity then
        return aIsCity
    end

    return (a.text or a.name or ""):lower() < (b.text or b.name or ""):lower()
end

function Tabs.InitialTabSelection(mainFrame, tabOrder, SelectTab)
    PanelTemplates_SetNumTabs(mainFrame, #tabOrder)
    local saved = GetCharSetting("lastTabID", nil)
    local idxToSelect = 1
    if saved then
        for i, id in ipairs(tabOrder) do
            if id == saved then idxToSelect = i break end
        end
    end
    PanelTemplates_SetTab(mainFrame, idxToSelect)
    if tabOrder and tabOrder[idxToSelect] then
        SelectTab(tabOrder[idxToSelect])
    end
end

-- Create summary bar (background + text)
function Summary.CreateSummaryBar(mainFrame, summaryY)
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
function Summary.UpdateSummary(mainFrame, collected, total)
    local percent = total > 0 and (collected / total * 100) or 0
    mainFrame.summaryText:SetFormattedText("Collected: %d / %d (%.2f%%)", collected, total, percent)
    local r, g, b = GetCompletionColor(percent)
    mainFrame.summaryBg:SetColorTexture(r, g, b, 0.34)
end

-- For expansion tabs
function Summary.UpdateExpansionSummary(mainFrame, expID)
    local instances = GetInstancesForExpansion(expID)
    local collected, total, percent = Util.GetCollectionProgress(instances)
    Summary.UpdateSummary(mainFrame, collected, total)
end

-- For zone tabs
function Summary.UpdateZoneSummary(mainFrame, tab, tabButtons)
    if not tab or not tab.zoneData then
        Summary.UpdateSummary(mainFrame, 0, 0)
        return
    end
    local node = tab.zoneData
    local collected, total = Util.ResolveProgress(node)
    Summary.UpdateSummary(mainFrame, collected, total)
end

-- Helper: Populate a frame with widgets in a grid
function Grid.Populate(content, dataset, widgetFactory, widgets, widgetSize, padding, scroll)
--    local __t0 = (debugprofilestop and debugprofilestop()) or (GetTimePreciseSec() * 1000)
    Util.ClearChildrenOrTabs(content)
    wipe(widgets)
    local frameWidth = scroll:GetWidth()
    local cols = Util.GetGridCols(frameWidth, widgetSize, padding) -- Calculate grid columns based on container width
    local x, y = 0, 0
    for _, entry in ipairs(dataset) do
        local f = widgetFactory(content, entry, x, y, widgetSize, padding)
        table.insert(widgets, f)
        x = x + 1
        if x >= cols then x = 0; y = y + 1 end
    end
    content:SetSize(frameWidth, (y + 1) * (60 + padding) + 80)
--    DebugFlushSoon("Grid.Populate")
--    local __t1 = (debugprofilestop and debugprofilestop()) or (GetTimePreciseSec() * 1000)
--    DebugLogf("[Perf] Populate(%s): %.2f ms", tostring(tab and tab.name or "?"), __t1 - __t0)
end

-- Factory: Create a scrolling grid for any dataset and widget factory
function Grid.Create(parent, dataset, widgetFactory, widgetSize, padding)
    local scroll = CreateFrame("ScrollFrame", nil, parent, "UIPanelScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", 10, -10)
    scroll:SetPoint("BOTTOMRIGHT", -30, 10)

    local content = CreateFrame("Frame", nil, scroll)
    content:SetSize(1, 1)
    scroll:SetScrollChild(content)

    local widgets = {}

    local function Populate()
        Grid.Populate(content, dataset, widgetFactory, widgets, widgetSize, padding, scroll)
    end
    local Debounced = Util.Debounce(Populate, 0.08)

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
    f:SetMovable(true)
    f:SetResizable(true)
    f:EnableMouse(true)

    if f.SetResizeBounds then f:SetResizeBounds(700, 360, 1600, 1200) end

    f:RegisterForDrag("LeftButton")
    f:SetScript("OnDragStart", f.StartMoving)
    f:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        Util.SaveFramePosition(self, "mainWindowPos")
    end)

    -- bottom-right resize grabber
    local resizer = CreateFrame("Button", nil, f)
    resizer:SetSize(16, 16)
    resizer:SetPoint("BOTTOMRIGHT", -6, 6)
    resizer:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")
    resizer:SetHighlightTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Highlight")
    resizer:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Down")
    resizer:SetScript("OnMouseDown", function() f:StartSizing("BOTTOMRIGHT") end)
    resizer:SetScript("OnMouseUp", function()
        f:StopMovingOrSizing()
        Util.SaveFramePosition(f, "mainWindowPos")
        if RefreshActiveTab then RefreshActiveTab() end
    end)

    -- also persist when size changes (e.g. via code)
    f:HookScript("OnSizeChanged", function(self) Util.SaveFramePosition(self, "mainWindowPos") end)

    f:Hide()
    f.TitleText:SetText("ATT-GoGo - Progress summaries")
end

-- Helper: Create gear icon button for options
local function CreateOptionsButton()
    local optionsBtn = CreateFrame("Button", nil, mainFrame)
    optionsBtn:SetSize(16, 16)
    optionsBtn:SetPoint("TOPRIGHT", mainFrame, "TOPRIGHT", -30, -3)
    optionsBtn:SetNormalTexture("Interface\\Icons\\INV_Misc_Gear_01")
    optionsBtn:SetHighlightTexture("Interface\\Icons\\INV_Misc_Gear_01", "ADD")
    Util.SetTooltip(optionsBtn, "ANCHOR_LEFT", "Options", "Open ATT-GoGo Options")
    optionsBtn:SetScript("OnClick", function() ShowATTGoGoOptions() end)
end


local function SelectTab(tabID)
    if currentTab then currentTab:Hide() end
    local tab = tabButtons[tabID]
    if tab and tab.content then
        SetCharSetting("lastTabID", tabID)
        tab.content:Show()
        currentTab = tab.content
--        DebugFlushSoon("tab-switch")
        -- Update summary for expansions
        if type(tabID) == "number" then
            Summary.UpdateExpansionSummary(mainFrame, tabID)
        else
            Summary.UpdateZoneSummary(mainFrame, tab, tabButtons)
        end
    end
end

--mainFrame:HookScript("OnHide", function()
--    if bookmarksFrame then bookmarksFrame:Hide() end
--end)
--
--function HideATTGoGoMain()
--    mainFrame:Hide()
--    if bookmarksFrame then bookmarksFrame:Hide() end
--end

function ShowATTGoGoMain()
    Util.LoadFramePosition(mainFrame, "mainWindowPos", "TOP", -36, -48)
    mainFrame:Show()
--    if bookmarksFrame then bookmarksFrame:Show() end
    for _, btn in pairs(tabButtons) do
        if btn.content then btn.content:Hide() end
    end
    -- pick saved tab if available, else first
    local saved = GetCharSetting("lastTabID", nil)
    local idxToSelect = 1
    if saved and tabOrder then
        for i, id in ipairs(tabOrder) do
            if id == saved then idxToSelect = i break end
        end
    end
    PanelTemplates_SetTab(mainFrame, idxToSelect)
    if tabOrder and tabOrder[idxToSelect] then
        SelectTab(tabOrder[idxToSelect])
    end
end

-- Helper: Register mainFrame events for updates/redraws
local function RegisterMainFrameEvents()
    mainFrame:RegisterEvent("UPDATE_INSTANCE_INFO")
    mainFrame:RegisterEvent("PLAYER_ENTERING_WORLD")
    mainFrame:RegisterEvent("PLAYER_LOGIN")
    mainFrame:SetScript("OnEvent", function()
        Util.ClearProgressCache()
        RefreshActiveTab()
    end)
end

-- Public: refresh the currently-visible grid (used by Options)
function RefreshActiveTab()
    if not (currentTab and currentTab:IsShown()) then return end
    -- Try to find the scroll frame we created in Grid.Create and call its Refresh()
    local kids = { currentTab:GetChildren() }
    for _, child in ipairs(kids) do
        if child and child.Refresh then
            Util.ClearProgressCache()
            child:Refresh()
            return
        end
    end
    -- Fallback: if the first child has an OnShow handler, invoke it
    if kids[1] and kids[1].GetScript then
        local onShow = kids[1]:GetScript("OnShow")
        if onShow then onShow(kids[1]) end
    end
end

function SetupMainUI()
    tabOrder = {}  -- tab ID lookup

    RequestRaidInfo()

    CreateMainFrame()
    CreateOptionsButton()
    table.insert(UISpecialFrames, "ATTGoGoMainFrame") -- Escape closes window
    Summary.CreateSummaryBar(mainFrame, summaryY)

    expansions = BuildExpansionList() -- dynamically built 1-based list
    local zones = BuildZoneList()

    Util.ClearChildrenOrTabs(tabButtons) -- Remove all old tabs/buttons/content from previous runs (in case of reload)
    Tabs.CreateTabs(mainFrame, tabButtons, tabOrder, expansions, expTabY, false, 1, SelectTab)
    Tabs.CreateTabs(mainFrame, tabButtons, tabOrder, zones, zoneTabY, true, #expansions + 1, SelectTab)

    Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, expansions, summaryY - 35, false, nil, nil, Grid.Create)
    Tabs.CreateTabContents(mainFrame, tabButtons, tabOrder, zones, -105, true,
        function(child) return child.mapID and tonumber(child.total or 0) > 0 end,
        Tabs.ZoneEntrySort, Grid.Create)

    Tabs.InitialTabSelection(mainFrame, tabOrder, SelectTab)
    RegisterMainFrameEvents()
end
