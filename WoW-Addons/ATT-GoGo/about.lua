-- === About Window =========================================================
local addonName = ...
ICON_FILE = GetAddOnMetadata(addonName, "X-IconFile")
TITLE = GetAddOnMetadata(addonName, "Title") or "UNKNOWN"
CTITLE = "|cff00ff00" .. TITLE .. "|r "

AboutUI = {
  frame = nil,
}

function SetupAboutFrame()
  if AboutUI.frame then return AboutUI.frame end

  local f = CreateFrame("Frame", "ATTGoGoAboutFrame", UIParent, "BasicFrameTemplateWithInset")
  f:SetSize(360, 260)
  f:Hide()

  -- Ensure it appears above other frames
  f:SetFrameStrata("FULLSCREEN_DIALOG")
  f:SetToplevel(true)
  f:EnableMouse(true)

  -- drag + persist position
  Util.EnableDragPersist(f, "aboutWindowPos")

  f.TitleText:SetText(TITLE .. " - About")

  -- Simple placeholder info text ? we'll flesh this out later
  local msg = f:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
  msg:SetPoint("TOPLEFT", 16, -32)
  msg:SetPoint("TOPRIGHT", -16, -32)
  msg:SetJustifyH("LEFT")
  msg:SetJustifyV("TOP")
  msg:SetText(
    "ATT-GoGo\n\n" ..
    "Companion addon for 'All The Things'.\n\n" ..
    "This About window is a placeholder.\n" ..
    "We will add more detailed information here later."
  )

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
