-- === About Window =========================================================
local addonName = ...

TITLE = GetAddOnMetadata(addonName, "Title")
CTITLE = "|cff40fd11" .. TITLE .. "|r "

local ADDON_PATH = "Interface\\AddOns\\" .. addonName .. "\\"
local QR_PATH = ADDON_PATH .. "QR\\"
local ICON_PPME = QR_PATH .. GetAddOnMetadata(addonName, "X-QrPayPalMe")
local ICON_DISC = QR_PATH .. GetAddOnMetadata(addonName, "X-QrDiscord")
local ICON_ATT  = QR_PATH .. GetAddOnMetadata(addonName, "X-QrDiscord-ATT")
ICON_MAIN = ADDON_PATH .. GetAddOnMetadata(addonName, "X-IconMain") .. ".tga"

AboutUI = {
  frame = nil,
}

local function cmd(c, a) return "|cffd0ffc0/" .. c .. "|r" .. (a and (" |cfff2f2a0" .. a .. "|r") or "") end

-- Build the slash-command section so About text and /gogo help stay in sync.
function BuildSlashCommandsText()
  local lines = {}

  table.insert(lines, cmd("gogo", "help") .. " - Show this help")
  table.insert(lines, cmd("gogo", "about") .. " - \"About\" window")
  table.insert(lines, cmd("gogo", "options") .. " - Options window")
  table.insert(lines, cmd("gogo", "show") .. " - Main window")
  table.insert(lines, cmd("gogo", "list") .. " - Uncollected List for current instance/zone")

  if GetSetting("DBG_en", false) == true then
    table.insert(lines, cmd("gogo", "dump") .. " - Debug: path + recursive dump for current context")
    table.insert(lines, cmd("gogo", "add <text>") .. " - Append <text> into debug log")
    table.insert(lines, cmd("gogo", "perf <0/1/reset>") .. " - Performance stats off/on/reset")
  end

  table.insert(lines, "You can also use " .. cmd("agg") .. " or " .. cmd("attgogo") .. " instead of " .. cmd("gogo"))

  return lines
end

local function BuildBodyText()
  local t = {}

  local function add(line) table.insert(t, line) end
  local function hdr(line) add("|cf486daff" .. line .. "|r") end

  add(CTITLE .. "is a companion addon for |cffffd200AllTheThings|r.")
  add("")
  hdr("Purpose")
  add("It focuses on answering \"where should I go next?\" for collectors who farm old and current content.")
  add("")
  hdr("What this addon is")
  add(" - A compact, account-aware overview of your dungeon and raid progress.")
  add(" - A fast Uncollected list for your current zone/instance, powered by ATT data.")
  add(" - A lightweight UI layer on top of ATT, not a replacement for ATT windows.")
  add("")
  hdr("Where it is useful")
  add(" - When you are standing at a raid or dungeon entrance and want to know if it is worth running on this character.")
  add(" - When planning a farming session and deciding which zone or instance to focus on.")
  add(" - When you like doing checklist-style runs across many characters.")
  add(" - When you want a quick zone overview without digging through the full ATT tree.")
  add("")
  hdr("Core features")
  add(" - Main grid window with tiles for each instance/zone, grouped by expansion and by continent.")
  add(" - Each tile shows collected vs. total collectibles and a color-coded completion bar.")
  add(" - Tooltip per tile with detailed ATT progress and (optionally) other-characters data, including active locks.")
  add(" - One-click Uncollected popup that lists remaining items, quests, achievements, etc.")
  add(" - Optional 3D model preview dock for uncollected creatures and a Dressing Room try-on helper.")
  add(" - Favorites system to pin your priority instances/zones to the top of the grid.")
  add(" - Snapshotting of instance and zone progress for \"other toons\" overlays in tooltips.")
  add(" - Optional minimap icon with quick access to the main grid, options, and the Uncollected popup.")
  add("")
  hdr("Typical usage")
  add(" - |cddeed200Farming session|r: open the main window, pick an expansion tab, and scan tiles for low % completion.")
  add(" - |cddeed200Instances|r: right-click the minimap icon or use " .. cmd("gogo") .. " list to see uncollected for that place.")
  add(" - |cddeed200Running content|r: leave the Uncollected popup open and let it auto-track zone/instance changes.")
  add(" - |cddeed200Alt farming|r: use the \"other characters\" tooltip option to see who has already finished an instance.")
  add("")
  hdr("What to expect")
  add(" - Most data (what exists, where it drops, what you already own) comes from |cffffd200AllTheThings|r.")
  add(" - " .. CTITLE .. "does not change how ATT tracks collections; it only reads ATT's data and presents it differently.")
  add(" - Performance is tuned for MoP Classic: grids, popups, and tooltips are virtualized and warmed up in small chunks.")
  add(" - Some heavy ATT queries may still take a moment on first open after login or after clearing the ATT cache, like getting list of uncollected items for collection-heavy places, e.g. capital cities.")
  add("")
  hdr("Who this is for")
  add(" - Players who already use |cffffd200AllTheThings|r and want a \"dashboard\" bird-eye view for dungeons, raids, and zones.")
  add(" - Mount, transmog, toy, and achievement farmers who do a lot of repeated runs.")
  add(" - People who enjoy planning checklists for entire expansions or for many alts at once.")
  add("")
  hdr("Basic usage")
  add(" - Open the main " .. CTITLE .. "window from the minimap button or via a slash command " .. cmd("gogo", "show") .. ".")
  add(" - Use the expansion tabs (top row) to switch between eras.")
  add(" - Use the zone tabs (second row) to drill into continents and their zones.")
  add(" - Set \"favorite\" tiles to sort them on top for quick access.")
  add(" - Mouse over a tile to see detailed progress in the tooltip, including optional lockouts and other-toons data.")
  add(" - Click a tile to open a popup of still uncollected items for that instance/zone.")
  add(" - Many items in the Uncollected popup are clickable to show their location on the World Map.")
  add(" - You can link most of those collectibles into chat with the standard SHIFT + Click action.")
  add(" - If a location of an item is known it will be highlighted on World Map when mouse-over on it in Uncollected popup.")
  add("")
  hdr("Slash commands")
  add(table.concat(BuildSlashCommandsText(), "\n"))
  add("")
  hdr("Notes")
  add(" - " .. CTITLE .. "is an independent companion project and not an official ATT module.")
  add(" - Relies on |cffffd200AllTheThings|r for data and services, so most features require ATT to be enabled.")
  add("")
  hdr("Support & community")
  add("If you find " .. CTITLE .. "useful, you can optionally leave a tip or join the Discord server.")
  add("Both options are kept at the very bottom of this page so they do not get in your way.")
  add("")
  hdr("Miscellaneous")
  add("Scan one of the codes below if you want to support the addon or join the community. Of course this is totally optional, so feel free to ignore this section entirely.")

  return table.concat(t, "\n")
end

function SetupAboutFrame()
  if AboutUI.frame then return AboutUI.frame end

  local f = CreateFrame("Frame", "ATTGoGoAboutFrame", UIParent, "BasicFrameTemplateWithInset")
  f:SetSize(720, 640)
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

  ---------------------------------------------------------------------------
  -- QR codes (PayPal + Discord) – placed low so they are out of immediate view
  ---------------------------------------------------------------------------
  local QR_SZ = 96
  local qrPayPal = content:CreateTexture(nil, "ARTWORK")
  qrPayPal:SetSize(QR_SZ, QR_SZ)
  qrPayPal:SetPoint("TOPLEFT", msg, "BOTTOMLEFT", 0, -16)
  qrPayPal:SetTexture(ICON_PPME)

  local qrPayPalLabel = content:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
  qrPayPalLabel:SetPoint("TOPLEFT", qrPayPal, "BOTTOMLEFT", 0, -4)
  qrPayPalLabel:SetText("PayPal.me/YePhIcK")

  local qrDiscord = content:CreateTexture(nil, "ARTWORK")
  qrDiscord:SetSize(QR_SZ, QR_SZ)
  qrDiscord:SetPoint("LEFT", qrPayPal, "RIGHT", 40, 0)
  qrDiscord:SetTexture(ICON_DISC)

  local qrDiscordLabel = content:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
  qrDiscordLabel:SetPoint("TOPLEFT", qrDiscord, "BOTTOMLEFT", 0, -4)
  qrDiscordLabel:SetText(TITLE .. " Discord\n|cee999999https://discord.gg/|r\nQWMSk9NaJv")

  local qrDiscordAtt = content:CreateTexture(nil, "ARTWORK")
  qrDiscordAtt:SetSize(QR_SZ, QR_SZ)
  qrDiscordAtt:SetPoint("LEFT", qrDiscord, "RIGHT", 40, 0)
  qrDiscordAtt:SetTexture(ICON_ATT)

  local qrDiscordAttLabel = content:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
  qrDiscordAttLabel:SetPoint("TOPLEFT", qrDiscordAtt, "BOTTOMLEFT", 0, -4)
  qrDiscordAttLabel:SetText("ATT Discord\n|cee999999https://discord.gg/|r\nallthethings")

  -- Make sure the scroll child is tall enough
  local totalHeight =
      msg:GetStringHeight()
      + 16 + QR_SZ
      + 8  + qrDiscordLabel:GetStringHeight()

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
