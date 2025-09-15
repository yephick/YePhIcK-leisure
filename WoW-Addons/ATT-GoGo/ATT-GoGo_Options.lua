-- === ATT-GoGo Options UI ===

local optionsFrame = CreateFrame("Frame", "ATTGoGoOptionsFrame", UIParent, "BasicFrameTemplateWithInset")
optionsFrame:SetSize(300, 570)

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

-- -----------------------------
-- Group box helper (Windows-like)
-- -----------------------------
local function CreateGroup(parent, label, width, point)
    local f = CreateFrame("Frame", nil, parent, BackdropTemplateMixin and "BackdropTemplate" or nil)
    f:SetWidth(width or 260)
    f:SetPoint(unpack(point)) -- caller anchors TOPLEFT
    f:SetBackdrop({
        bgFile   = "Interface\\ChatFrame\\ChatFrameBackground",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile     = true, tileSize = 16, edgeSize = 12,
        insets   = { left = 4, right = 4, top = 8, bottom = 4 },
    })
    f:SetBackdropColor(0, 0, 0, 0.25)

    f.label = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    f.label:SetPoint("TOPLEFT", f, "TOPLEFT", 12, 4)
    f.label:SetText(label)

    -- cover the top border under the label (Windows-style group title)
    f.labelBG = f:CreateTexture(nil, "ARTWORK", nil, 1)
    f.labelBG:SetColorTexture(0.05, 0.05, 0.05, 1)
    f.labelBG:SetPoint("LEFT",  f.label, "LEFT",  -6, 0)
    f.labelBG:SetPoint("RIGHT", f.label, "RIGHT",  6, 0)
    f.labelBG:SetHeight(16)                               -- tall enough to hide the border line

    return f
end

-- Generic checkbox factory with optional tooltip (description-only)
-- point: { "TOPLEFT", anchorFrame, "BOTTOMLEFT", 0, -6 } or { "TOPLEFT", 12, -24 }
local function AddCheckbox(parent, label, point, getValue, setValue, onChange, ...)
    local cb = CreateFrame("CheckButton", nil, parent, "ChatConfigCheckButtonTemplate")
    cb:SetPoint(unpack(point))
    cb.Text:SetText(label)

    -- Tooltip lines (no title/header)
    local lines = { ... }
    if #lines > 0 then
        Util.SetTooltip(cb, "ANCHOR_LEFT", "", unpack(lines))
    end

    cb:SetScript("OnShow", function()
        cb:SetChecked(getValue() and true or false)
    end)
    cb:SetScript("OnClick", function(self)
        local v = self:GetChecked() and true or false
        setValue(v)
        if onChange then onChange(v) end
    end)
    return cb
end

-- =============================
-- Account-wide group
-- =============================
local accountGroup = CreateGroup(optionsFrame, "Account-wide", 260, { "TOPLEFT", 20, -36 })

local minimapCheckbox = AddCheckbox(
    accountGroup,
    "Show minimap icon",
    { "TOPLEFT", accountGroup, "TOPLEFT", 12, -16 },
    function()
        return not (ATTGoGoDB and ATTGoGoDB.minimap and ATTGoGoDB.minimap.hide)
    end,
    function(v)
        ATTGoGoDB = ATTGoGoDB or {}
        ATTGoGoDB.minimap = ATTGoGoDB.minimap or {}
        ATTGoGoDB.minimap.hide = (not v)
        local icon = LibStub:GetLibrary("LibDBIcon-1.0", true)
        if icon then if v then icon:Show("ATT-GoGo") else icon:Hide("ATT-GoGo") end end
    end,
    nil,
    "Shows a movable launcher icon near the minimap."
)

local instIconCheckbox = AddCheckbox(
    accountGroup,
    "Show instance icon on widgets",
    { "TOPLEFT", minimapCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("showInstanceIconOnWidgets", true) end,
    function(v) SetSetting("showInstanceIconOnWidgets", v) end,
    function() if RefreshActiveTab then RefreshActiveTab() end end,
    "Adds the instance’s icon to each tile."
)

local removedCheckbox = AddCheckbox(
    accountGroup,
    "Include unobtainable content",
    { "TOPLEFT", instIconCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("includeRemoved", false) end,
    function(v) SetSetting("includeRemoved", v) end,
    function()
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then ShowUncollectedPopup(popup.currentData) end
        RefreshActiveTab()
    end,
    "Include removed/retired/future content in the uncollected popup list."
)

local hover3DCheckbox = AddCheckbox(
    accountGroup,
    "3D hover preview",
    { "TOPLEFT", removedCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("showHover3DPreview", true) end,
    function(v)
        SetSetting("showHover3DPreview", v)
        local dock = _G.ATTGoGoPreviewDock
        if dock and not v then dock:Hide() end
    end,
    nil,
    "Show 3D model when hovering mouse over uncollected creatures."
)

local nakedTryOnCheckbox = AddCheckbox(
    accountGroup,
    "Try-on items on a naked model",
    { "TOPLEFT", hover3DCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("dressUpNaked", true) end,
    function(v) SetSetting("dressUpNaked", v) end,
    nil,
    "When ON, undress the character first in Dressing Room.",
    "When OFF, layer the item onto the current outfit."
)

local autoRefreshPopupCheckbox = AddCheckbox(
    accountGroup,
    "Popup tracks zone/instance change",
    { "TOPLEFT", nakedTryOnCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("autoRefreshPopupOnZone", true) end,
    function(v) SetSetting("autoRefreshPopupOnZone", v) end,
    function(v)
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
    "If the Uncollected popup is open, retarget it when you change zone or enter an instance."
)

-- === Other-toons-in-tooltips dropdown (account-wide) ===
local otherToonsLabel = accountGroup:CreateFontString(nil, "OVERLAY", "GameFontNormal")
otherToonsLabel:SetPoint("TOPLEFT", autoRefreshPopupCheckbox, "BOTTOMLEFT", 0, -8)
otherToonsLabel:SetText("Show other characters in tooltips")

local otherToonsDD = CreateFrame("Frame", "ATTGoGoOtherToonsDropdown", accountGroup, "UIDropDownMenuTemplate")
otherToonsDD:SetPoint("TOPLEFT", otherToonsLabel, "BOTTOMLEFT", -12, -4)

local OT_CHOICES = {
  { text = "Don’t show", value = 0 },
  { text = "Only with lockouts (instances)", value = 1 },
  { text = "Show all (zones & instances)", value = 2 },
}

UIDropDownMenu_SetWidth(otherToonsDD, 210)

local function SyncOtherToonsDropdown()
  local v = Util.GetOtherToonsMode()
  UIDropDownMenu_SetSelectedValue(otherToonsDD, v)
  -- Also set the visible text so it never shows "Custom"
  for _, opt in ipairs(OT_CHOICES) do
    if opt.value == v then
      UIDropDownMenu_SetText(otherToonsDD, opt.text)
      break
    end
  end
end

UIDropDownMenu_Initialize(otherToonsDD, function(self, level)
  for _, opt in ipairs(OT_CHOICES) do
    local info = UIDropDownMenu_CreateInfo()
    info.text = opt.text
    info.value = opt.value
    info.checked = (opt.value == Util.GetOtherToonsMode())
    info.func = function()
      SetSetting("otherToonsInTooltips", tonumber(opt.value) or 1)
      SyncOtherToonsDropdown()
    end
    UIDropDownMenu_AddButton(info, level)
  end
end)

otherToonsDD:SetScript("OnShow", SyncOtherToonsDropdown)

accountGroup:SetHeight(24 + (6 * 20) + (5 * 6) + 18 + 12 + 50)

-- =============================
-- Per-character group
-- =============================
local perCharGroup = CreateGroup(optionsFrame, "Per-character", 260, { "TOPLEFT", 20, - (32 + accountGroup:GetHeight() + 16) })

local criteriaCheckbox = AddCheckbox(
    perCharGroup,
    "List individual achievement criteria",
    { "TOPLEFT", perCharGroup, "TOPLEFT", 12, -16 },
    function() return GetCharSetting("expandAchievementCriteria", false) end,
    function(v) SetCharSetting("expandAchievementCriteria", v) end,
    function()
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then ShowUncollectedPopup(popup.currentData) end
    end,
    "When ON, show every uncompleted criterion separately.",
    "When OFF, show only the parent achievement."
)

local groupVisualsCheckbox = AddCheckbox(
    perCharGroup,
    "Group items by appearance",
    { "TOPLEFT", criteriaCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetCharSetting("groupByVisualID", true) end,
    function(v) SetCharSetting("groupByVisualID", v) end,
    function()
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then ShowUncollectedPopup(popup.currentData) end
    end,
    "Collapse duplicate appearances and show one representative item."
)

---- Filters ----
local filterLabel = perCharGroup:CreateFontString(nil, "OVERLAY", "GameFontNormal")
filterLabel:SetPoint("TOPLEFT", groupVisualsCheckbox, "BOTTOMLEFT", 0, -12)
filterLabel:SetText("Include in uncollected popup:")

local filterCheckboxes = {}

local COLLECTIBLE_ID_ORDER = {
    "achievementID", "creatureID", "explorationID", "flightpathID", "gearSetID",
    "itemID", "mapID", "questID", "titleID", "visualID"
}

local colWidth, rowHeight = 120, 24
local i = 1
for _, v in ipairs(COLLECTIBLE_ID_ORDER) do
    local col = (i-1) % 2
    local row = math.floor((i-1) / 2)
    local cb = CreateFrame("CheckButton", "ATTGoGoFilterCheckbox_"..v, perCharGroup, "InterfaceOptionsCheckButtonTemplate")
    cb.key = v
    if row == 0 then
        cb:SetPoint("TOPLEFT", filterLabel, "BOTTOMLEFT", col*colWidth, -6)
    else
        cb:SetPoint("TOPLEFT", filterLabel, "BOTTOMLEFT", col*colWidth, -6 - row*rowHeight)
    end
    cb.Text:SetText(COLLECTIBLE_ID_LABELS[v] or v)
    cb.Text:SetPoint("LEFT", cb, "RIGHT", 4, 0)
    cb:SetChecked(Util.GetPopupIdFilters()[v])
    cb:SetScript("OnClick", function(self)
        local val = self:GetChecked() and true or false
        Util.SetPopupIdFilter(self.key, val)
        local popup = _G.ATTGoGoUncollectedPopup
        if popup and popup:IsShown() and popup.currentData then ShowUncollectedPopup(popup.currentData) end
    end)
    Util.SetTooltip(cb, "ANCHOR_RIGHT", "", "Include " .. (COLLECTIBLE_ID_LABELS[v] or v) .. " entries in the popup.")
    filterCheckboxes[v] = cb
    i = i + 1
end

-- Size per-character group to enclose its content
perCharGroup:SetHeight(24 + 2*20 + 12 + (math.ceil(#COLLECTIBLE_ID_ORDER/2) * rowHeight) + 18 + 12)

-- update these checkboxes from DB (for re-opening the window)
local function UpdateFilterCheckboxes()
    local effective = Util.GetPopupIdFilters()
    for k, cb in pairs(filterCheckboxes) do
        if type(k) == "string" then cb:SetChecked(effective[k]) end
    end
end

-- --- Reset window sizes/positions ---
local resetBtn = CreateFrame("Button", nil, optionsFrame, "UIPanelButtonTemplate")
resetBtn:SetSize(190, 22)
resetBtn:SetPoint("BOTTOM", optionsFrame, "BOTTOM", 0, 12)
resetBtn:SetText("Reset window sizes/positions")
Util.SetTooltip(resetBtn, "ANCHOR_TOPLEFT",
    "Reset window sizes/positions",
    "Clear saved sizes/positions for the main, popup, and options windows and restore defaults.")

resetBtn:SetScript("OnClick", function()
    -- wipe saved positions/sizes
    ATTGoGoCharDB = ATTGoGoCharDB or {}
    ATTGoGoCharDB.mainWindowPos   = nil
    ATTGoGoCharDB.popupWindowPos  = nil
    ATTGoGoCharDB.optionsWindowPos= nil

    -- Main window: default size + default anchor
    if _G.ATTGoGoMainFrame then
        _G.ATTGoGoMainFrame:SetSize(724, 612)
        Util.LoadFramePosition(_G.ATTGoGoMainFrame, "mainWindowPos", "TOP", -36, -48)
        if _G.ATTGoGoMainFrame:IsShown() and RefreshActiveTab then RefreshActiveTab() end
    end
    local popup = _G.ATTGoGoUncollectedPopup
    if popup then
        popup:SetSize(268, 592)
        Util.LoadFramePosition(popup, "popupWindowPos", "RIGHT", -200, 64)
    end
    optionsFrame:SetSize(300, 570)
    Util.LoadFramePosition(optionsFrame, "optionsWindowPos", "LEFT", 92, 80)
    print("|cff00ff00[ATT-GoGo]|r Window sizes/positions reset to defaults.")
end)

optionsFrame:HookScript("OnShow", function()
    local function Refresh(cb)
        if cb and cb.GetScript then local f = cb:GetScript("OnShow"); if f then f(cb) end end
    end
    Refresh(minimapCheckbox)
    Refresh(instIconCheckbox)
    Refresh(nakedTryOnCheckbox)
    Refresh(removedCheckbox)
    Refresh(criteriaCheckbox)
    Refresh(groupVisualsCheckbox)
    UpdateFilterCheckboxes()
end)

-- Show options window function
function ShowATTGoGoOptions()
    if ATTGoGoMainFrame then
        optionsFrame:SetFrameStrata("DIALOG")
    end
    Util.LoadFramePosition(optionsFrame, "optionsWindowPos", "LEFT", 92, 80)
    optionsFrame:Show()
end

table.insert(UISpecialFrames, "ATTGoGoOptionsFrame")
