-- === ATT-GoGo Options UI ===

local optionsFrame = CreateFrame("Frame", "ATTGoGoOptionsFrame", UIParent, "BasicFrameTemplateWithInset")
optionsFrame:SetSize(300, 380)

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
 
-- Generic checkbox factory:
-- point: { "TOPLEFT", 20, -35 } or { "TOPLEFT", anchorFrame, "BOTTOMLEFT", 0, -8 }
-- getValue: function() -> boolean
-- setValue: function(boolean)
-- onChange: optional function(boolean)
local function AddCheckbox(parent, label, point, getValue, setValue, onChange)
    local cb = CreateFrame("CheckButton", nil, parent, "ChatConfigCheckButtonTemplate")
    cb:SetPoint(unpack(point))
    cb.Text:SetText(label)
    cb:SetScript("OnShow", function()
        local ok, val = pcall(getValue); if ok then cb:SetChecked(val and true or false) end
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
    end
)

-- Checkbox: show instance icon on widgets
local instIconCheckbox = AddCheckbox(
    optionsFrame,
    "Show instance icon on widgets",
    { "TOPLEFT", minimapCheckbox, "BOTTOMLEFT", 0, -8 },
    function() return GetSetting("showInstanceIconOnWidgets", true) end,
    function(v) SetSetting("showInstanceIconOnWidgets", v) end,
    function() if RefreshActiveTab then RefreshActiveTab() end end
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
    end
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
    end
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
    end
)

-- === Filter Options (Dynamic Checkboxes) ===
local COLLECTIBLE_ID_ORDER = {
    "achievementID", "creatureID", "explorationID", "flightpathID", "gearSetID",
    "instanceID", "itemID", "mapID", "questID", "titleID", "visualID"
}

local filterLabel = optionsFrame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
filterLabel:SetPoint("TOPLEFT", groupVisualsCheckbox, "BOTTOMLEFT", 0, -15)
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
    filterCheckboxes[v] = cb   -- store by logical key only
    i = i + 1
end

-- Optionally, add a function to update these checkboxes from DB (for re-opening the window)
local function UpdateFilterCheckboxes()
    local effective = Util.GetPopupIdFilters()
    for k, cb in pairs(filterCheckboxes) do
        if type(k) == "string" then
            cb:SetChecked(effective[k])
        end
    end
end

optionsFrame:HookScript("OnShow", function()
    -- trigger each checkbox's OnShow to re-read current values
    local function Refresh(cb) if cb and cb.GetScript then local f = cb:GetScript("OnShow"); if f then f(cb) end end end
    Refresh(minimapCheckbox)
    Refresh(instIconCheckbox)
    Refresh(criteriaCheckbox)
    Refresh(removedCheckbox)
    Refresh(groupVisualsCheckbox)
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

