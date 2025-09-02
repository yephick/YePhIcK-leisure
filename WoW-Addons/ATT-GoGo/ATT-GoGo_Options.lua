-- === ATT-GoGo Options UI ===

local optionsFrame = CreateFrame("Frame", "ATTGoGoOptionsFrame", UIParent, "BasicFrameTemplateWithInset")
optionsFrame:SetSize(300, 500)

optionsFrame:SetMovable(true)
optionsFrame:EnableMouse(true)
optionsFrame:RegisterForDrag("LeftButton")
optionsFrame:SetScript("OnDragStart", optionsFrame.StartMoving)
optionsFrame:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    Util.SaveFramePosition(optionsFrame, "optionsWindowPos")
end)
optionsFrame:Hide()

-- Title
optionsFrame.TitleText:SetText("ATT-GoGo Options")
 
-- Generic checkbox factory with optional tooltip:
-- point:     { "TOPLEFT", 20, -35 } or { "TOPLEFT", anchorFrame, "BOTTOMLEFT", 0, -8 }
-- getValue:  function() -> boolean
-- setValue:  function(boolean)
-- onChange:  optional function(boolean)
-- ...:       optional strings (tooltip lines)
local function AddCheckbox(parent, label, point, getValue, setValue, onChange, ...)
    local cb = CreateFrame("CheckButton", nil, parent, "ChatConfigCheckButtonTemplate")
    cb:SetPoint(unpack(point))
    cb.Text:SetText(label)

    -- Tooltip lines (no title/header)
    local lines = { ... }
    if Util and Util.SetTooltip and #lines > 0 then
        -- skip AddHeader, only add lines
        Util.SetTooltip(cb, "ANCHOR_LEFT", "", unpack(lines))
    end

    cb:SetScript("OnShow", function()
        local ok, val = pcall(getValue)
        if ok then cb:SetChecked(val and true or false) end
    end)
    cb:SetScript("OnClick", function(self)
        local v = self:GetChecked() and true or false
        pcall(setValue, v)
        if onChange then pcall(onChange, v) end
    end)
    return cb
end

-- Checkbox: minimap icon (special DB path + LibDBIcon show/hide)
local minimapCheckbox = AddCheckbox(
    optionsFrame,
    "Show minimap icon",
    { "TOPLEFT", 20, -35 },
    function()
        return not (ATTGoGoDB and ATTGoGoDB.minimap and ATTGoGoDB.minimap.hide)
    end,
    function(v)
        ATTGoGoDB = ATTGoGoDB or {}
        ATTGoGoDB.minimap = ATTGoGoDB.minimap or {}
        ATTGoGoDB.minimap.hide = (not v)
        local icon = LibStub:GetLibrary("LibDBIcon-1.0", true)
        if icon then
            if v then icon:Show("ATT-GoGo") else icon:Hide("ATT-GoGo") end
        end
    end,
    nil,
    "Shows a movable launcher icon near the minimap."
)

-- Checkbox: show instance icon on widgets
local instIconCheckbox = AddCheckbox(
    optionsFrame,
    "Show instance icon on widgets",
    { "TOPLEFT", minimapCheckbox, "BOTTOMLEFT", 0, -8 },
    function() return GetSetting("showInstanceIconOnWidgets", true) end,
    function(v) SetSetting("showInstanceIconOnWidgets", v) end,
    function() if RefreshActiveTab then RefreshActiveTab() end end,
    "Adds the instance’s icon to each tile."
)

-- Checkbox: list individual achievement criteria (per-character)
local criteriaCheckbox = AddCheckbox(
    optionsFrame,
    "List individual achievement criteria",
    { "TOPLEFT", instIconCheckbox, "BOTTOMLEFT", 0, -8 },
    function() return GetCharSetting("expandAchievementCriteria", false) end,
    function(v) SetCharSetting("expandAchievementCriteria", v) end,
    function()
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then
            ShowUncollectedPopup(popup.currentData)
        end
    end,
    "When ON, show every uncompleted criterion separately.",
    "When OFF, only show the parent achievement for individual criteria."

)

-- Checkbox: include removed/retired content (account-wide)
local removedCheckbox = AddCheckbox(
    optionsFrame,
    "Include removed/retired content",
    { "TOPLEFT", criteriaCheckbox, "BOTTOMLEFT", 0, -8 },
    function() return GetSetting("includeRemoved", false) end,
    function(v) SetSetting("includeRemoved", v) end,
    function()
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then
            ShowUncollectedPopup(popup.currentData)
        end
    end,
    "Include removed/retired content in the uncollected popup list"
)

-- Checkbox: group items by appearance (visualID) — per-character (default ON)
local groupVisualsCheckbox = AddCheckbox(
    optionsFrame,
    "Group items by appearance (visualID)",
    { "TOPLEFT", removedCheckbox, "BOTTOMLEFT", 0, -8 },
    function() return GetCharSetting("groupByVisualID", true) end,
    function(v) SetCharSetting("groupByVisualID", v) end,
    function()
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then
            ShowUncollectedPopup(popup.currentData)
        end
    end,
    "Group items by appearance (visualID)"
)

-- Checkbox: 3D hover preview (account-wide)
local hover3DCheckbox = AddCheckbox(
    optionsFrame,
    "3D hover preview",
    { "TOPLEFT", groupVisualsCheckbox, "BOTTOMLEFT", 0, -8 },
    function() return GetSetting("showHover3DPreview", true) end,
    function(v)
        SetSetting("showHover3DPreview", v)
        local dock = _G.ATTGoGoPreviewDock
        if dock and not v then dock:Hide() end
    end,
    nil,
    "Show 3D model when hovering mouse over uncollected creatures"
)

-- Checkbox: try-on items on a naked model (account-wide, default ON)
local nakedTryOnCheckbox = AddCheckbox(
    optionsFrame,
    "Try-on items on a naked model",
    { "TOPLEFT", hover3DCheckbox, "BOTTOMLEFT", 0, -8 },
    function() return GetSetting("dressUpNaked", true) end,
    function(v) SetSetting("dressUpNaked", v) end,
    nil,
    "When ON, undress the chracter when viewing the item in Dressing Room, so only that one item is presented",
    "When OFF, only apply that one item to the model in Dressing Room"
)

-- Checkbox: auto-refresh Uncollected popup on zone/instance change (account-wide)
local autoRefreshPopupCheckbox = AddCheckbox(
    optionsFrame,
    "Popup follows zone/instance change",
    { "TOPLEFT", nakedTryOnCheckbox, "BOTTOMLEFT", 0, -12 },
    function() return GetSetting("autoRefreshPopupOnZone", true) end,
    function(v) SetSetting("autoRefreshPopupOnZone", v) end,
    function(v)
        -- If user just turned it ON and popup is open, refresh immediately to current context
        if v then
            C_Timer.After(0.05, function()
                local popup = _G.ATTGoGoUncollectedPopup
                if popup and popup:IsShown() then
                    local node, info = Util.ResolveContextNode(true)
                    if info and info.kind == "zone" then
                        local strict = ResolveContainerZoneNodeStrict(info.uiMapID)
                        if strict then ShowUncollectedPopup(strict) return end
                    end
                    if node then ShowUncollectedPopup(node) end
                end
            end)
        end
    end,
    "If the Uncollected popup is open, update it automatically when you change zone or enter an instance."
)

-- === Filter Options (Dynamic Checkboxes) ===
local COLLECTIBLE_ID_ORDER = {
    "achievementID", "creatureID", "explorationID", "flightpathID", "gearSetID",
    "instanceID", "itemID", "mapID", "questID", "titleID", "visualID"
}

local filterLabel = optionsFrame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
filterLabel:SetPoint("TOPLEFT", autoRefreshPopupCheckbox, "BOTTOMLEFT", 0, -15)
filterLabel:SetText("Show in info popup:")

local filterCheckboxes = {}

local colWidth, rowHeight = 160, 24
local i = 1
for _, v in ipairs(COLLECTIBLE_ID_ORDER) do
    local col = (i-1) % 2
    local row = math.floor((i-1) / 2)
    local cb = CreateFrame("CheckButton", "ATTGoGoFilterCheckbox_"..v, optionsFrame, "InterfaceOptionsCheckButtonTemplate")
    cb.key = v
    if row == 0 then
        cb:SetPoint("TOPLEFT", filterLabel, "BOTTOMLEFT", col*colWidth, -8)
    else
        cb:SetPoint("TOPLEFT", filterLabel, "BOTTOMLEFT", col*colWidth, -8 - row*rowHeight)
    end
    cb.Text:SetText(COLLECTIBLE_ID_LABELS[v] or v)
    cb.Text:SetPoint("LEFT", cb, "RIGHT", 4, 0)
    cb:SetChecked(Util.GetPopupIdFilters()[v])
    cb:SetScript("OnClick", function(self)
        local val = self:GetChecked() and true or false
        Util.SetPopupIdFilter(self.key, val)
    
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then
            ShowUncollectedPopup(popup.currentData)
        end
    end)
    Util.SetTooltip(cb, "ANCHOR_RIGHT", "", "Include " .. (COLLECTIBLE_ID_LABELS[v] or v) .. " entries in the popup.")
    filterCheckboxes[v] = cb   -- store by logical key only
    i = i + 1
end

-- update these checkboxes from DB (for re-opening the window)
local function UpdateFilterCheckboxes()
    local effective = Util.GetPopupIdFilters()
    for k, cb in pairs(filterCheckboxes) do
        if type(k) == "string" then
            cb:SetChecked(effective[k])
        end
    end
end

-- --- Reset window sizes/positions ---
local resetBtn = CreateFrame("Button", nil, optionsFrame, "UIPanelButtonTemplate")
resetBtn:SetSize(190, 22)
resetBtn:SetPoint("BOTTOMLEFT", 12, 12)
resetBtn:SetText("Reset window sizes/positions")
if Util and Util.SetTooltip then
    Util.SetTooltip(resetBtn, "ANCHOR_TOPLEFT",
        "Reset window sizes/positions",
        "Clear saved sizes/positions for the main, popup, and options windows and restore defaults.")
end

resetBtn:SetScript("OnClick", function()
    -- wipe saved positions/sizes
    ATTGoGoCharDB = ATTGoGoCharDB or {}
    ATTGoGoCharDB.mainWindowPos   = nil
    ATTGoGoCharDB.popupWindowPos  = nil
    ATTGoGoCharDB.optionsWindowPos= nil

    -- Main window: default size + default anchor
    if _G.ATTGoGoMainFrame then
        _G.ATTGoGoMainFrame:SetSize(724, 612)                       -- default size
        Util.LoadFramePosition(_G.ATTGoGoMainFrame, "mainWindowPos", "TOP", -36, -48)  -- default anchor
        if _G.ATTGoGoMainFrame:IsShown() and RefreshActiveTab then RefreshActiveTab() end
    end

    -- Popup window: ensure it exists before touching it
    local popup = _G.ATTGoGoUncollectedPopup
    if popup then
        popup:SetSize(268, 592)                                      -- default size
        Util.LoadFramePosition(popup, "popupWindowPos", "RIGHT", -200, 64) -- default anchor
    end

    -- Options window (this one)
    optionsFrame:SetSize(300, 500)                                   -- default size
    Util.LoadFramePosition(optionsFrame, "optionsWindowPos", "LEFT", 92, 80) -- default anchor

    print("|cff00ff00[ATT-GoGo]|r Window sizes/positions reset to defaults.")
end)

optionsFrame:HookScript("OnShow", function()
    -- trigger each checkbox's OnShow to re-read current values
    local function Refresh(cb) if cb and cb.GetScript then local f = cb:GetScript("OnShow"); if f then f(cb) end end end
    Refresh(minimapCheckbox)
    Refresh(instIconCheckbox)
    Refresh(criteriaCheckbox)
    Refresh(removedCheckbox)
    Refresh(groupVisualsCheckbox)
    Refresh(nakedTryOnCheckbox)
    UpdateFilterCheckboxes()
end)

-- Show options window function
function ShowATTGoGoOptions()
    if ATTGoGoMainFrame then
        optionsFrame:SetFrameStrata("DIALOG") -- Explicitly set to DIALOG to ensure it’s above mainFrame
    end
    Util.LoadFramePosition(optionsFrame, "optionsWindowPos", "LEFT", 92, 80)
    optionsFrame:Show()
end

table.insert(UISpecialFrames, "ATTGoGoOptionsFrame")

