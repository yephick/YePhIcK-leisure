-- ATT-GoGo_Widget.lua

Widget = {}

-- Whole-widget click + hover border + hand cursor
function Widget.AttachClickAndHoverUX(f, data)
    -- Click anywhere on the widget to open the popup
    f:EnableMouse(true)
    f:SetScript("OnMouseUp", function(self, button)
        if button == "LeftButton" or button == "MiddleButton" then
            ShowUncollectedPopup(data)
        end
    end)

    -- Cache original border color once
    local function cacheOriginals(self)
        if not self.__origBorderColor then
            local r, g, b, a = self:GetBackdropBorderColor()
            self.__origBorderColor = { r or 0, g or 0, b or 0, a or 1 }
        end
    end

    -- Hover: gold border + hand cursor
    f:HookScript("OnEnter", function(self)
        cacheOriginals(self)
        self:SetBackdropBorderColor(1, 0.82, 0, 1)   -- gold-ish
        SetCursor("Interface\\CURSOR\\Point")
    end)

    f:HookScript("OnLeave", function(self)
        if self.__origBorderColor then
            self:SetBackdropBorderColor(
                self.__origBorderColor[1], self.__origBorderColor[2],
                self.__origBorderColor[3], self.__origBorderColor[4]
            )
        end
        ResetCursor()
    end)
end

function Widget.SetProgressWidgetVisuals(f, data, percent, isZone)
  local r, g, b = GetCompletionColor(percent)
  f:SetBackdropColor(r, g, b, 0.85)
  local br, bg, bb = math.min(r * 2.2, 1), math.min(g * 2.2, 1), math.min(b * 2.2, 1)
  f:SetBackdropBorderColor(br, bg, bb, 1)
  f:SetAlpha(1)
  if not isZone then
    local isLocked, numDown, numBosses = IsInstanceLockedOut(data)
    if isLocked then
      local allDead = numBosses and numBosses > 0 and numDown == numBosses
      if allDead or numBosses == 0 then
        f:SetBackdropColor(0.25, 0.25, 0.25, 0.35)
        f:SetBackdropBorderColor(0.22, 0.22, 0.22, 0.70)
        f:SetAlpha(0.40)
      else
        f:SetBackdropBorderColor(0.8, 0.85, 0.93, 1)
      end
    end
  end
end
function Widget.AddProgressWidgetText(f, data, widgetSize, collected, total, percent, attNode)
  local title = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
  title:SetPoint("TOP", 0, -10)
  title:SetJustifyH("CENTER")
  title:SetWidth(widgetSize - 8)
  title:SetText(Util.NodeDisplayName(data))
  title:SetWordWrap(false)
  title:SetMaxLines(1)
  if data.instanceID  then
    local isLocked, _, _, lockoutIndex = IsInstanceLockedOut(data)
    if isLocked and lockoutIndex then
      local reset = select(3, GetSavedInstanceInfo(lockoutIndex))
      if reset > 0 then
        local lockFS = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        lockFS:SetPoint("TOP", title, "BOTTOM", 0, -2)
        lockFS:SetJustifyH("CENTER")
        lockFS:SetWidth(widgetSize - 8)
        lockFS:SetText("|cffffd200" .. Util.FormatLockoutTime(reset) .. "|r")
      end
    end
  end
  local stats = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
  stats:SetPoint("BOTTOM", 0, 8)
  stats:SetJustifyH("CENTER")
  stats:SetWidth(widgetSize - 8)
  stats:SetText(string.format("%d / %d (%.1f%%)", collected, total, percent))
end

function Widget.SetProgressWidgetTooltip(f, data, collected, total, percent, isZone)
  Tooltip.CreateTooltip(f, "ANCHOR_RIGHT", function()
    Tooltip.AddLine(Util.NodeDisplayName(data))
    Tooltip.AddProgress(GameTooltip, data, collected, total, percent, isZone, data)
  end)
end

-- Main: Create a progress widget for grid
function Widget.CreateProgressWidget(content, data, x, y, widgetSize, padding, isZone, attNode)
    local f = CreateFrame("Frame", nil, content, BackdropTemplateMixin and "BackdropTemplate" or nil)
    f:SetSize(widgetSize, 60)
    f:SetPoint("TOPLEFT", x * (widgetSize + padding), -y * (60 + padding))

    f:SetBackdrop({
        bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 20,
        insets = { left = 3, right = 3, top = 3, bottom = 3 }
    })

    -- Instance icon in top-left if available
    if GetSetting("showInstanceIconOnWidgets", true) and data.instanceID then
        local instNode = Util.ATTFindInstanceByInstanceID(data.instanceID)
        if instNode then
            local tex = f:CreateTexture(nil, "ARTWORK")
            tex:SetSize(48, 48)
            tex:SetPoint("TOPLEFT", f, "TOPLEFT", 6, -6)
            Util.ApplyNodeIcon(tex, instNode, { texCoord = { 0.07, 0.93, 0.07, 0.93 } })
        end
    end

    local nodeForProgress = attNode or data
    local collected, total, percent = Util.ResolveProgress(nodeForProgress)
    Widget.SetProgressWidgetVisuals(f, data, percent, isZone)
    Widget.AddProgressWidgetText(f, data, widgetSize, collected, total, percent, attNode)
    Widget.SetProgressWidgetTooltip(f, data, collected, total, percent, isZone)
    Widget.AttachClickAndHoverUX(f, attNode or data)

    return f
end

