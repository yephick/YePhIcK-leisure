-- === About Window =========================================================
local addonName = ...

TITLE = GetAddOnMetadata(addonName, "Title")
CTITLE = "|cff00ff00" .. TITLE .. "|r "

local ADDON_PATH = "Interface\\AddOns\\" .. addonName .. "\\"
ICON_MAIN = ADDON_PATH .. GetAddOnMetadata(addonName, "X-IconMain") .. ".tga"
ICON_PPME = ADDON_PATH .. GetAddOnMetadata(addonName, "X-PayPalMeQR")
ICON_DISC = ADDON_PATH .. GetAddOnMetadata(addonName, "X-DiscordQR")

AboutUI = {
  frame = nil,
}

-- Build the slash-command section so About text and /gogo help stay in sync.
function BuildSlashCommandsText()
  local lines = {}

  table.insert(lines, "/gogo help - Show this help")
  table.insert(lines, "/gogo about - \"About\" window")
  table.insert(lines, "/gogo options - Options window")
  table.insert(lines, "/gogo show - Main window")
  table.insert(lines, "/gogo list - Uncollected List for current instance/zone")

  if GetSetting("TP_en", false) == true then
    table.insert(lines, "/gogo dump - Debug: path + recursive dump for current context")
    table.insert(lines, "/gogo add <text> - Append <text> into debug log")
    table.insert(lines, "/gogo perf <0/1/reset> - Performance stats off/on/reset")
  end

  table.insert(lines, "You can also use /agg or /attgogo instead of /gogo")

  return lines
end

local function BuildBodyText()
  local t = {}

  local function add(line)
    table.insert(t, line)
  end

  add(CTITLE .. "is a companion addon for |cffffd200AllTheThings|r.")
  add("It focuses on dungeon and raid collection progress and shows it in a compact grid by expansion or zone.")
  add("")
  add("|cffffff00What it does|r")
  add(" - Shows per-instance/per-zone tiles with collected versus missing achievements, appearances, quests, and other collectibles.")
  add(" - Lets you quickly see where you still have work to do.")
  add(" - Helps you decide where to go next when you feel like farming.")
  add(" - Uses |cffffd200AllTheThings|r data and services, so most features require ATT to be enabled.")
  add("")
  add("|cffffff00Basic usage|r")
  add(" - Open the main " .. CTITLE .. "window from the minimap button or via a slash command.")
  add(" - Use the expansion tabs to switch between eras. Or choose a continent to see its zones.")
  add(" - Mouse over a tile to see detailed progress in the tooltip. Instances show lockouts for this and other toons.")
  add(" - Click a tile to open a popup of still uncollected items.")
  add("")
  add("|cffffff00Slash commands|r")
  add(table.concat(BuildSlashCommandsText(), "\n"))
  add("")
  add("|cffffff00Notes|r")
  add(" - " .. CTITLE .. "is an independent companion project and not an official ATT module.")
  add("")
  add("|cffffff00Support & community|r")
  add("If you find " .. CTITLE .. "useful, you can optionally leave a tip or join the Discord server.")
  add("Both options are kept at the very bottom of this page so they do not get in your way.")

  return table.concat(t, "\n")
end

function SetupAboutFrame()
  if AboutUI.frame then return AboutUI.frame end

  local f = CreateFrame("Frame", "ATTGoGoAboutFrame", UIParent, "BasicFrameTemplateWithInset")
  f:SetSize(640, 480)
  f:Hide()

  -- ensure it appears above other frames
  f:SetFrameStrata("FULLSCREEN_DIALOG")
  f:SetToplevel(true)
  f:EnableMouse(true)

  -- drag + persist position
  Util.EnableDragPersist(f, "aboutWindowPos")

  f.TitleText:SetText(TITLE .. " - About")

  ---------------------------------------------------------------------------
  -- Scrollable content area
  ---------------------------------------------------------------------------
  local scrollFrame = CreateFrame("ScrollFrame", "$parentScrollFrame", f, "UIPanelScrollFrameTemplate")
  scrollFrame:SetPoint("TOPLEFT", 16, -32)
  scrollFrame:SetPoint("BOTTOMRIGHT", -30, 16)

  scrollFrame:EnableMouseWheel(true)
  scrollFrame:SetScript("OnMouseWheel", function(self, delta)
    local step = 24
    local current = self:GetVerticalScroll()
    local maxRange = self:GetVerticalScrollRange()
    local newValue = current - delta * step
    if newValue < 0 then
        newValue = 0
    elseif newValue > maxRange then
        newValue = maxRange
    end

    self:SetVerticalScroll(newValue)
  end)

  local contentWidth = f:GetWidth() - 56

  local content = CreateFrame("Frame", nil, scrollFrame)
  content:SetWidth(contentWidth)
  scrollFrame:SetScrollChild(content)

  -- Main text
  local msg = content:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
  msg:SetPoint("TOPLEFT", 0, 0)
  msg:SetWidth(contentWidth)
  msg:SetJustifyH("LEFT")
  msg:SetJustifyV("TOP")
  msg:SetText(BuildBodyText())

  -- Support & Community header is already part of the body text; here we
  -- only add the QR section *below* the text so it starts off-screen.
  local qrHeader = content:CreateFontString(nil, "OVERLAY", "GameFontNormal")
  qrHeader:SetPoint("TOPLEFT", msg, "BOTTOMLEFT", 0, -32)
  qrHeader:SetText("|cffffff00Optional QR codes|r")

  local qrSubText = content:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
  qrSubText:SetPoint("TOPLEFT", qrHeader, "BOTTOMLEFT", 0, -4)
  qrSubText:SetWidth(contentWidth)
  qrSubText:SetJustifyH("LEFT")
  qrSubText:SetText(
    "Scan one of the codes below if you want to support the addon or join the community. " ..
    "Of course this is totally optional, so feel free to ignore this section entirely."
  )

  ---------------------------------------------------------------------------
  -- QR codes (PayPal + Discord) – placed low so they are out of immediate view
  ---------------------------------------------------------------------------
  local qrPayPal = content:CreateTexture(nil, "ARTWORK")
  qrPayPal:SetSize(96, 96)
  qrPayPal:SetPoint("TOPLEFT", qrSubText, "BOTTOMLEFT", 0, -16)
  qrPayPal:SetTexture(ICON_PPME)

  local qrPayPalLabel = content:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
  qrPayPalLabel:SetPoint("TOPLEFT", qrPayPal, "BOTTOMLEFT", 0, -4)
  qrPayPalLabel:SetText("PayPal.me/YePhIcK")

  local qrDiscord = content:CreateTexture(nil, "ARTWORK")
  qrDiscord:SetSize(96, 96)
  qrDiscord:SetPoint("LEFT", qrPayPal, "RIGHT", 40, 0)
  qrDiscord:SetTexture(ICON_DISC)

  local qrDiscordLabel = content:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
  qrDiscordLabel:SetPoint("TOPLEFT", qrDiscord, "BOTTOMLEFT", 0, -4)
  qrDiscordLabel:SetText("ATT-GoGo Discord")

  -- Make sure the scroll child is tall enough
  local totalHeight =
      msg:GetStringHeight()
      + 32 + qrHeader:GetStringHeight()
      + 4  + qrSubText:GetStringHeight()
      + 16 + 96  -- QR height
      + 8  + qrPayPalLabel:GetStringHeight()

  content:SetHeight(totalHeight)

  f:SetScript("OnHide", function(self) Util.SaveFramePosition(self, "aboutWindowPos") end)

  AboutUI.frame = f
  table.insert(UISpecialFrames, "ATTGoGoAboutFrame") -- Esc to close
  return f
end

function AboutUI.Show()
  local f = SetupAboutFrame()
  Util.LoadFramePosition(f, "aboutWindowPos", "CENTER", 0, 0)
  f:Show()
  f:Raise()
end

function AboutUI.Hide()
  if AboutUI.frame then
    AboutUI.frame:Hide()
  end
end
