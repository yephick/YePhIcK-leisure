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

-- Checkbox for minimap icon
local minimapCheckbox = CreateFrame("CheckButton", "ATTGoGoShowMinimapCheckbox", optionsFrame, "ChatConfigCheckButtonTemplate")
minimapCheckbox:SetPoint("TOPLEFT", 20, -35)
minimapCheckbox.Text:SetText("Show minimap icon")

local function UpdateMinimapCheckbox()
    minimapCheckbox:SetChecked(not (ATTGoGoDB and ATTGoGoDB.minimap and ATTGoGoDB.minimap.hide))
end

local function SaveMinimapCheckbox()
    local checked = minimapCheckbox:GetChecked()
    ATTGoGoDB = ATTGoGoDB or {}
    ATTGoGoDB.minimap = ATTGoGoDB.minimap or {}
    ATTGoGoDB.minimap.hide = not checked
    local icon = LibStub:GetLibrary("LibDBIcon-1.0", true)
    if icon then
        if checked then
            icon:Show("ATT-GoGo")
        else
            icon:Hide("ATT-GoGo")
        end
    end
end

minimapCheckbox:SetScript("OnClick", SaveMinimapCheckbox)

-- Checkbox: show instance icon on widgets
local instIconCheckbox = CreateFrame("CheckButton", "ATTGoGoWidgetInstIconCheckbox", optionsFrame, "ChatConfigCheckButtonTemplate")
instIconCheckbox:SetPoint("TOPLEFT", minimapCheckbox, "BOTTOMLEFT", 0, -8)
instIconCheckbox.Text:SetText("Show instance icon on widgets")

local function UpdateInstIconCheckbox()
    -- default true if not set
    instIconCheckbox:SetChecked(GetSetting("showInstanceIconOnWidgets", true))
end

local function SaveInstIconCheckbox()
    local checked = instIconCheckbox:GetChecked() and true or false
    SetSetting("showInstanceIconOnWidgets", checked)

    RefreshActiveTab()
end

instIconCheckbox:SetScript("OnClick", SaveInstIconCheckbox)

-- Checkbox: expand/collapse achievement criteria in popup (per-character)
local criteriaCheckbox = CreateFrame("CheckButton", "ATTGoGoExpandCriteriaCheckbox", optionsFrame, "ChatConfigCheckButtonTemplate")
criteriaCheckbox:SetPoint("TOPLEFT", instIconCheckbox, "BOTTOMLEFT", 0, -8)
criteriaCheckbox.Text:SetText("List individual achievement criteria")  -- unchecked = collapse (default)

local function UpdateCriteriaCheckbox()
    criteriaCheckbox:SetChecked(GetCharSetting("expandAchievementCriteria", false))
end

local function SaveCriteriaCheckbox()
    local val = criteriaCheckbox:GetChecked() and true or false
    SetCharSetting("expandAchievementCriteria", val)

    -- If the popup is open, refresh it to reflect the new behavior
    local popup = _G.ATTGoGoUncollectedPopup
    if popup and popup:IsShown() and popup.currentData then
        ShowUncollectedPopup(popup.currentData)
    end
end

criteriaCheckbox:SetScript("OnClick", SaveCriteriaCheckbox)

-- Checkbox: include removed/retired content (account-wide)
local removedCheckbox = CreateFrame("CheckButton", "ATTGoGoIncludeRemovedCheckbox", optionsFrame, "ChatConfigCheckButtonTemplate")
removedCheckbox:SetPoint("TOPLEFT", criteriaCheckbox, "BOTTOMLEFT", 0, -8)
removedCheckbox.Text:SetText("Include removed/retired content")

local function UpdateRemovedCheckbox()
    removedCheckbox:SetChecked(GetSetting("includeRemoved", false))
end

local function SaveRemovedCheckbox()
    local val = removedCheckbox:GetChecked() and true or false
    SetSetting("includeRemoved", val)
    -- If the popup is open, refresh to reflect new behavior
    local popup = _G.ATTGoGoUncollectedPopup
    if popup and popup:IsShown() and popup.currentData then
        ShowUncollectedPopup(popup.currentData)
    end
end

removedCheckbox:SetScript("OnClick", SaveRemovedCheckbox)

-- Checkbox: group items with same appearance (per-character, default = ON)
local groupVisualsCheckbox = CreateFrame("CheckButton", "ATTGoGoGroupVisualsCheckbox", optionsFrame, "ChatConfigCheckButtonTemplate")
groupVisualsCheckbox:SetPoint("TOPLEFT", removedCheckbox, "BOTTOMLEFT", 0, -8)
groupVisualsCheckbox.Text:SetText("Group items by appearance (visualID)")

local function UpdateGroupVisualsCheckbox()
    groupVisualsCheckbox:SetChecked(GetCharSetting("groupByVisualID", true))
end

local function SaveGroupVisualsCheckbox()
    local val = groupVisualsCheckbox:GetChecked() and true or false
    SetCharSetting("groupByVisualID", val)
    -- If the popup is open, refresh to reflect new behavior
    local popup = _G.ATTGoGoUncollectedPopup
    if popup and popup:IsShown() and popup.currentData then
        ShowUncollectedPopup(popup.currentData)
    end
end

groupVisualsCheckbox:SetScript("OnClick", SaveGroupVisualsCheckbox)

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
        local prevCheckbox = filterCheckboxes[i-2]
        if prevCheckbox then
            cb:SetPoint("TOPLEFT", prevCheckbox, "BOTTOMLEFT", 0, -2)
        else
            -- Fallback anchoring to avoid nil reference
            cb:SetPoint("TOPLEFT", filterLabel, "BOTTOMLEFT", col*colWidth, -8 - row*rowHeight)
        end
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
    UpdateMinimapCheckbox()
    UpdateInstIconCheckbox()
    UpdateCriteriaCheckbox()
    UpdateRemovedCheckbox()
    UpdateGroupVisualsCheckbox()
    UpdateFilterCheckboxes()
end)

-- Show options window function
function ShowATTGoGoOptions()
    if ATTGoGoMainFrame then
        optionsFrame:SetFrameStrata("DIALOG") -- Explicitly set to DIALOG to ensure it’s above mainFrame
    end
    Util.LoadFramePosition(optionsFrame, "optionsWindowPos", "LEFT", 92, 80)
    UpdateMinimapCheckbox()
    optionsFrame:Show()
end

table.insert(UISpecialFrames, "ATTGoGoOptionsFrame")

