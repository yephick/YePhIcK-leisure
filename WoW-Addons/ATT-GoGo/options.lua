-- === Options UI ===

-- Local module state ---------------------------------------------------------
OptionsUI = {
  frame = nil,
  accountGroup = nil,
  perCharGroup = nil,
  filterCheckboxes = {},
  controls = {},
}

-- Forward decls --------------------------------------------------------------
local CreateGroup, AddCheckbox

-- Frame factory --------------------------------------------------------------
local function SetupOptionsFrame()
--  if OptionsUI.frame then return OptionsUI.frame end
  local f = CreateFrame("Frame", "ATTGoGoOptionsFrame", UIParent, "BasicFrameTemplateWithInset")
  f:SetSize(300, 570)
  f:Hide()
  Util.EnableDragPersist(f, "optionsWindowPos")                                     -- replaces the custom drag code

  f.TitleText:SetText(TITLE .. " options")

  -- Reset window sizes/positions button
  local resetBtn = CreateFrame("Button", nil, f, "UIPanelButtonTemplate")
  resetBtn:SetSize(190, 22)
  resetBtn:SetPoint("BOTTOM", f, "BOTTOM", 0, 12)
  resetBtn:SetText("Reset window sizes/positions")
  Util.SetTooltip(resetBtn, "ANCHOR_TOPLEFT",
    "Reset window sizes/positions",
    "Clear saved sizes/positions for the main, popup, and options windows and restore defaults.")
  resetBtn:SetScript("OnClick", function()
    ATTGoGoCharDB = ATTGoGoCharDB or {}
    ATTGoGoCharDB.mainWindowPos    = nil
    ATTGoGoCharDB.popupWindowPos   = nil
    ATTGoGoCharDB.optionsWindowPos = nil

    -- Main window defaults
    _G.ATTGoGoMainFrame:SetSize(724, 612)
    Util.LoadFramePosition(_G.ATTGoGoMainFrame, "mainWindowPos", "TOP", -36, -48)
    RefreshActiveTab()

    -- Popup defaults
    local popup = _G.ATTGoGoUncollectedPopup
    popup:SetSize(268, 592)
    Util.LoadFramePosition(popup, "popupWindowPos", "RIGHT", -200, 64)

    -- Options defaults (self)
    Util.LoadFramePosition(f, "optionsWindowPos", "LEFT", 92, 80)

    print(CTITLE .. "Window sizes/positions reset to defaults.")
  end)

  -- When the window opens, sync controls from DB
  f:HookScript("OnShow", function()
    local function RefreshControl(cb) cb:GetScript("OnShow")(cb) end
    for _, key in ipairs({
      "minimapCheckbox","instIconCheckbox","nakedTryOnCheckbox",
      "removedCheckbox","criteriaCheckbox","groupVisualsCheckbox",
    }) do
      RefreshControl(OptionsUI.controls[key])
    end
    -- Dropdown sync
    OptionsUI.controls.SyncOtherToonsDropdown()
    -- Filter grid sync
    OptionsUI.UpdateFilterCheckboxes()
  end)

  OptionsUI.frame = f
  table.insert(UISpecialFrames, "ATTGoGoOptionsFrame") -- Esc closes
  return f
end

-- Group helpers --------------------------------------------------------------
CreateGroup = function(parent, label, width, point)
  local f = CreateFrame("Frame", nil, parent, BackdropTemplateMixin and "BackdropTemplate" or nil)
  f:SetWidth(width or 260)
  f:SetPoint(unpack(point))
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

  -- hide the top border under the label (Windows-like)
  f.labelBG = f:CreateTexture(nil, "ARTWORK", nil, 1)
  f.labelBG:SetColorTexture(0.05, 0.05, 0.05, 1)
  f.labelBG:SetPoint("LEFT",  f.label, "LEFT",  -6, 0)
  f.labelBG:SetPoint("RIGHT", f.label, "RIGHT",  6, 0)
  f.labelBG:SetHeight(16)

  return f
end

AddCheckbox = function(parent, label, point, getValue, setValue, onChange, ...)
  local cb = CreateFrame("CheckButton", nil, parent, "ChatConfigCheckButtonTemplate")
  cb:SetPoint(unpack(point))
  cb.Text:SetText(label)

  local lines = { ... }
  if #lines > 0 then
    Util.SetTooltip(cb, "ANCHOR_LEFT", "", unpack(lines))
  end

  cb:SetScript("OnShow", function() cb:SetChecked(bool(getValue())) end)
  cb:SetScript("OnClick", function(self)
    local v = bool(self:GetChecked())
    setValue(v)
    if onChange then onChange(v) end
  end)
  return cb
end

-- Account-wide group ---------------------------------------------------------
function OptionsUI.BuildAccountGroup(parent)
  local g = CreateGroup(parent, "Account-wide", 260, { "TOPLEFT", 20, -36 })
  OptionsUI.accountGroup = g

  -- Minimap icon
  local minimapCheckbox = AddCheckbox(
    g,
    "Show minimap icon",
    { "TOPLEFT", g, "TOPLEFT", 12, -16 },
    function() return not ATTGoGoDB.minimap.hide end,
    function(v)
      ATTGoGoDB.minimap.hide = (not v)
      local icon = LibStub:GetLibrary("LibDBIcon-1.0", true)
      if v then icon:Show(TITLE) else icon:Hide(TITLE) end
    end,
    nil,
    "Shows a movable launcher icon near the minimap."
  )
  OptionsUI.controls.minimapCheckbox = minimapCheckbox

  -- Instance icon on widgets
  local instIconCheckbox = AddCheckbox(
    g,
    "Show instance/zone icon on widgets",
    { "TOPLEFT", minimapCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("showInstanceIconOnWidgets", true) end,
    function(v) SetSetting("showInstanceIconOnWidgets", v) end,
    RefreshActiveTab,
    "Adds the instance’s icon to each tile."
  )
  OptionsUI.controls.instIconCheckbox = instIconCheckbox

  -- Include unobtainable content
  local removedCheckbox = AddCheckbox(
    g,
    "Include unobtainable content",
    { "TOPLEFT", instIconCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("includeRemoved", false) end,
    function(v) SetSetting("includeRemoved", v) end,
    function()
      local popup = _G.ATTGoGoUncollectedPopup
      if popup:IsShown() then ShowUncollectedPopup(popup.currentData) end
      RefreshActiveTab()
    end,
    "Include removed/retired/future content in the uncollected popup list."
  )
  OptionsUI.controls.removedCheckbox = removedCheckbox

  -- 3D hover preview
  local hover3DCheckbox = AddCheckbox(
    g,
    "3D hover preview",
    { "TOPLEFT", removedCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("showHover3DPreview", true) end,
    function(v)
      SetSetting("showHover3DPreview", v)
      if not v then _G.ATTGoGoPreviewDock:Hide() end
    end,
    nil,
    "Show 3D model when hovering mouse over uncollected creatures."
  )
  OptionsUI.controls.hover3DCheckbox = hover3DCheckbox

  -- Try-on on naked model
  local nakedTryOnCheckbox = AddCheckbox(
    g,
    "Try-on items on a naked model",
    { "TOPLEFT", hover3DCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("dressUpNaked", true) end,
    function(v) SetSetting("dressUpNaked", v) end,
    nil,
    "When ON, undress the character first in Dressing Room.",
    "When OFF, layer the item onto the current outfit."
  )
  OptionsUI.controls.nakedTryOnCheckbox = nakedTryOnCheckbox

  -- Auto-refresh popup tracking
  local autoRefreshPopupCheckbox = AddCheckbox(
    g,
    "Popup tracks zone/instance change",
    { "TOPLEFT", nakedTryOnCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetSetting("autoRefreshPopupOnZone", true) end,
    function(v) SetSetting("autoRefreshPopupOnZone", v) end,
    nil,
    "If the Uncollected popup is open, retarget it when you change zone or enter an instance."
  )
  OptionsUI.controls.autoRefreshPopupCheckbox = autoRefreshPopupCheckbox

  -- Other-toons dropdown
  local otherToonsLabel = g:CreateFontString(nil, "OVERLAY", "GameFontNormal")
  otherToonsLabel:SetPoint("TOPLEFT", autoRefreshPopupCheckbox, "BOTTOMLEFT", 0, -8)
  otherToonsLabel:SetText("Show other characters in tooltips")

  local otherToonsDD = CreateFrame("Frame", "ATTGoGoOtherToonsDropdown", g, "UIDropDownMenuTemplate")
  otherToonsDD:SetPoint("TOPLEFT", otherToonsLabel, "BOTTOMLEFT", -12, -4)
  UIDropDownMenu_SetWidth(otherToonsDD, 210)

  local OT_CHOICES = {
    { text = "Don’t show", value = 0 },
    { text = "Only with lockouts (instances)", value = 1 },
    { text = "Show all (zones & instances)", value = 2 },
  }

  local function SyncOtherToonsDropdown()
    local v = Util.GetOtherToonsMode()
    UIDropDownMenu_SetSelectedValue(otherToonsDD, v)
    for _, opt in ipairs(OT_CHOICES) do
      if opt.value == v then UIDropDownMenu_SetText(otherToonsDD, opt.text) break end
    end
  end
  OptionsUI.controls.SyncOtherToonsDropdown = SyncOtherToonsDropdown

  UIDropDownMenu_Initialize(otherToonsDD, function(self, level)
    for _, opt in ipairs(OT_CHOICES) do
      local info = UIDropDownMenu_CreateInfo()
      info.text = opt.text
      info.value = opt.value
      info.checked = (opt.value == Util.GetOtherToonsMode())
      info.func = function()
        SetSetting("otherToonsInTooltips", opt.value or 1)
        SyncOtherToonsDropdown()
      end
      UIDropDownMenu_AddButton(info, level)
    end
  end)
  otherToonsDD:SetScript("OnShow", SyncOtherToonsDropdown)
  OptionsUI.controls.otherToonsDD = otherToonsDD

  -- Final group height
  g:SetHeight(24 + (6 * 20) + (5 * 6) + 18 + 12 + 50)
end

-- Per-character group --------------------------------------------------------
function OptionsUI.BuildPerCharGroup(parent)
  -- Anchor this group directly under the Account group, not by frame height
  local anchor = OptionsUI.accountGroup or parent
  local g = CreateGroup(parent, "Per-character", 260, { "TOPLEFT", anchor, "BOTTOMLEFT", 0, -16 })
  OptionsUI.perCharGroup = g

  -- Expand achievement criteria
  local criteriaCheckbox = AddCheckbox(
    g,
    "List individual achievement criteria",
    { "TOPLEFT", g, "TOPLEFT", 12, -16 },
    function() return GetCharSetting("expandAchievementCriteria", false) end,
    function(v) SetCharSetting("expandAchievementCriteria", v) end,
    function()
      local popup = _G.ATTGoGoUncollectedPopup
      if popup:IsShown() then ShowUncollectedPopup(popup.currentData) end
    end,
    "When ON, show every uncompleted criterion separately.",
    "When OFF, show only the parent achievement."
  )
  OptionsUI.controls.criteriaCheckbox = criteriaCheckbox

  -- Group items by visualID
  local groupVisualsCheckbox = AddCheckbox(
    g,
    "Group items by appearance",
    { "TOPLEFT", criteriaCheckbox, "BOTTOMLEFT", 0, -6 },
    function() return GetCharSetting("groupByVisualID", true) end,
    function(v) SetCharSetting("groupByVisualID", v) end,
    function()
      local popup = _G.ATTGoGoUncollectedPopup
      if popup:IsShown() then ShowUncollectedPopup(popup.currentData) end
    end,
    "Collapse duplicate appearances and show one representative item."
  )
  OptionsUI.controls.groupVisualsCheckbox = groupVisualsCheckbox

  -- Filters grid
  OptionsUI.BuildFilterCheckboxes(g, groupVisualsCheckbox)
end

-- Filters -------------------------------------------------------------------
function OptionsUI.BuildFilterCheckboxes(group, anchor)
  local filterLabel = group:CreateFontString(nil, "OVERLAY", "GameFontNormal")
  filterLabel:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, -12)
  filterLabel:SetText("Include in uncollected popup:")

  local ORDER = {
    "achievementID", "creatureID", "explorationID", "flightpathID",
    "itemID", "mapID", "questID", "titleID", "visualID",
  }

  local colWidth, rowHeight = 120, 24
  for i, key in ipairs(ORDER) do
    local col = (i-1) % 2
    local row = math.floor((i-1) / 2)
    local cb = CreateFrame("CheckButton", "ATTGoGoFilterCheckbox_"..key, group, "InterfaceOptionsCheckButtonTemplate")
    cb.key = key
    cb:SetPoint("TOPLEFT", filterLabel, "BOTTOMLEFT", col*colWidth, -6 - row*rowHeight)
    cb.Text:SetText(COLLECTIBLE_ID_LABELS[key] or key)
    cb.Text:SetPoint("LEFT", cb, "RIGHT", 4, 0)
    cb:SetChecked(ATTGoGoCharDB.popupIdFilters[key])
    cb:SetScript("OnClick", function(self)
      Util.SetPopupIdFilter(self.key, bool(self:GetChecked()))
      local popup = _G.ATTGoGoUncollectedPopup
      if popup:IsShown() then ShowUncollectedPopup(popup.currentData) end
    end)
    Util.SetTooltip(cb, "ANCHOR_RIGHT", "", "Include "..(COLLECTIBLE_ID_LABELS[key] or key).." entries in the popup.")
    OptionsUI.filterCheckboxes[key] = cb
  end

  group:SetHeight(24 + 2*20 + 12 + (math.ceil(#ORDER/2) * rowHeight) + 18 + 12)
end

function OptionsUI.UpdateFilterCheckboxes()
  local effective = ATTGoGoCharDB.popupIdFilters
  for key, cb in pairs(OptionsUI.filterCheckboxes) do
    cb:SetChecked(effective[key])
  end
end

-- Public entry points --------------------------------------------------------
function OptionsUI.Init()
  local f = SetupOptionsFrame()
  OptionsUI.BuildAccountGroup(f)
  OptionsUI.BuildPerCharGroup(f)
  return f
end

function OptionsUI.Show()
  local f = OptionsUI.frame
  Util.LoadFramePosition(f, "optionsWindowPos", "LEFT", 92, 80)
  f:SetFrameStrata("DIALOG")
  f:Show()
end
