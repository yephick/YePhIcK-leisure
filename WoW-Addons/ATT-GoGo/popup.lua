-- popup.lua
-- Virtualized uncollected-list popup: fast, low-GC, and flicker-free.

------------------------------------------------------------
-- Module locals / state
------------------------------------------------------------
local uncollectedPopup          -- Frame (name: ATTGoGoUncollectedPopup)
local currentTooltipNode        -- last node whose tooltip is active
local bossItemsDock             -- text list shown beside the 3D boss preview

-- Lazy-resolution registries
local itemLabelsByID  = {}      -- [itemID]      = { label = FontString, btn = ItemButton }
local achLabelsByID   = {}      -- [achievement] = FontString
local spellLabelsByID = {}      -- [spellID]     = FontString

-- Which filter keys made a node pass (weak so we don't hold nodes alive)
local passKeysByNode  = setmetatable({}, { __mode = "k" })

-- Virtual list constants
local ROW_HEIGHT = 22           -- row height
local ROW_BTN_SZ = 20           -- ItemButton size
local ROW_BUFFER = 6            -- render-ahead buffer
local __rowSerial = 0           -- unique names for ItemButtonTemplate rows

local function SafeNodeName(n)
    return n.text or n.name or "?"
end

-- cached faction result
local FACTION = Util.PlayerFactionID()
local OPPOSITE_FACTION = (FACTION == 1 and 2) or (FACTION == 2 and 1) or 0

-- Cached player class ID (ATT uses: 1=Warrior, 2=Paladin, 3=Hunter, 4=Rogue, 5=Priest,
-- 6=Death Knight, 7=Shaman, 8=Mage, 9=Warlock, 10=Monk, 11=Druid, 12=Demon Hunter, 13=Evoker)
local CLASS_ID = select(3, UnitClass("player"))

local INCLUDE_REMOVED = false       -- set per-run in BuildNodeList
local ACTIVE_KEYS = nil             -- set per-run in BuildNodeList
local CanShowItemForCharacter
local CollectBossItems

local function IsAllowedLeaf(node, activeKeys)
    if node.visible == false or node.collected then return false, nil end

    if OPPOSITE_FACTION ~= 0 and node.r == OPPOSITE_FACTION then
        return false, nil
    end

    -- Class gate (ATT 'c' field)
    local nc = node.c
    if nc ~= nil then
        local ok = false
        if type(nc) == "table" then
            for i = 1, #nc do if nc[i] == CLASS_ID then ok = true; break end end
        else
            ok = (nc == CLASS_ID)
        end
        if not ok then return false, nil end
    end

    if node.itemID and not CanShowItemForCharacter(node, "main") then return false, nil end

    if not INCLUDE_REMOVED and Util.IsNodeRemoved(node) then return false, nil end

    -- quick sanity check: ANY match (no allocations)
    local anyMatch = false
    local ak       = ACTIVE_KEYS           -- cache upvalues to locals
    local n        = #ak
    for i = 1, n do
        local k = ak[i]
        local v = node[k]
        if v and v ~= 0 then anyMatch = true; break end
    end
    if not anyMatch then
        return false, nil
    end

    -- build the 'matched' list we store for emitted nodes
    local matched = {}
    for i = 1, #ACTIVE_KEYS do
        local k = ACTIVE_KEYS[i]
        local v = node[k]
        if v ~= nil and v ~= 0 then matched[#matched + 1] = k end
    end
    return #matched > 0, matched
end

local RETRIEVING = "Retrieving data"
local function IsPlaceholderTitle(t)
    return t == nil or t == "" or t == RETRIEVING or t:lower():find("retrieving")
end

-- Short display name for a collectible leaf
local function NodeShortName(n)
    local t = n and (n.text or n.name)
    if not IsPlaceholderTitle(t) then return t end
    if n.itemID  then return GetItemInfo(n.itemID) or "Item " .. n.itemID end
    if n.spellID then return GetSpellInfo(n.spellID) or ("Spell " .. n.spellID) end
    if n.questID then return C_QuestLog.GetQuestInfo(n.questID) or ("Quest " .. n.questID) end
    if n.titleID then return "Title " .. n.titleID end
    if n.achievementID then
        local _, nm = GetAchievementInfo(n.achievementID)
        return nm or ("Achievement " .. n.achievementID)
    end
    if n.creatureID or n.npcID then
        local c = Util.ATTSearchOne("creatureID", n.creatureID) or Util.ATTSearchOne("npcID", n.npcID)
        return c and c.name or ("Creature " .. (n.creatureID or n.npcID))
    end
    TP(n)
    return "Collectible"
end

local INVTYPE_LABELS = {
    INVTYPE_HEAD = "Head",
    INVTYPE_NECK = "Neck",
    INVTYPE_SHOULDER = "Shoulder",
    INVTYPE_BODY = "Shirt",
    INVTYPE_CHEST = "Chest",
    INVTYPE_ROBE = "Chest",
    INVTYPE_WAIST = "Waist",
    INVTYPE_LEGS = "Legs",
    INVTYPE_FEET = "Feet",
    INVTYPE_WRIST = "Wrist",
    INVTYPE_HAND = "Hands",
    INVTYPE_FINGER = "Finger",
    INVTYPE_TRINKET = "Trinket",
    INVTYPE_CLOAK = "Back",
    INVTYPE_WEAPON = "One-Hand",
    INVTYPE_SHIELD = "Off-Hand",
    INVTYPE_2HWEAPON = "Two-Hand",
    INVTYPE_WEAPONMAINHAND = "Main Hand",
    INVTYPE_WEAPONOFFHAND = "Off Hand",
    INVTYPE_HOLDABLE = "Off-Hand",
    INVTYPE_RANGED = "Ranged",
    INVTYPE_RANGEDRIGHT = "Ranged",
    INVTYPE_THROWN = "Thrown",
    INVTYPE_TABARD = "Tabard",
}

local bindingTooltip
local function BossItemBindPrefix(node)
    if not (node and node.itemID) then return "" end

    bindingTooltip = bindingTooltip or CreateFrame("GameTooltip", "ATTGoGoBindingTooltip", UIParent, "GameTooltipTemplate")
    bindingTooltip:SetOwner(UIParent, "ANCHOR_NONE")
    bindingTooltip:ClearLines()
    bindingTooltip:SetHyperlink("item:" .. node.itemID)

    local bindOnEquip = ITEM_BIND_ON_EQUIP and ITEM_BIND_ON_EQUIP:lower()
    local bindOnPickup = ITEM_BIND_ON_PICKUP and ITEM_BIND_ON_PICKUP:lower()
    for i = 2, bindingTooltip:NumLines() do
        local line = _G["ATTGoGoBindingTooltipTextLeft" .. i]
        local text = line and line:GetText()
        text = text and text:lower()
        if text == bindOnEquip then
            bindingTooltip:Hide()
            return "BoE "
        end
        if text == bindOnPickup then
            bindingTooltip:Hide()
            return "BoP "
        end
    end

    bindingTooltip:Hide()
    return ""
end

local function BossItemDisplayText(item)
    local bindPrefix = BossItemBindPrefix(item)
    local _, itemType, itemSubType, itemEquipLoc, _, classID = GetItemInfoInstant(item.itemID)
    local slot = INVTYPE_LABELS[itemEquipLoc] or (itemEquipLoc and _G[itemEquipLoc])
    local subType = itemSubType
    if subType == "" or subType == itemType then subType = nil end
    if classID == LE_ITEM_CLASS_WEAPON then
        slot = nil
    end

    if bindPrefix ~= "" then
        local parts = { (bindPrefix:gsub("%s+$", "")) }
        if slot then parts[#parts + 1] = slot end
        if subType then parts[#parts + 1] = subType end
        return "- " .. NodeShortName(item) .. " [" .. table.concat(parts, ", ") .. "]"
    end

    if slot and subType then
        return "- " .. NodeShortName(item) .. " [" .. slot .. ", " .. subType .. "]"
    end
    if slot then
        return "- " .. NodeShortName(item) .. " [" .. slot .. "]"
    end
    if subType then
        return "- " .. NodeShortName(item) .. " [" .. subType .. "]"
    end
    return "- " .. NodeShortName(item)
end

------------------------------------------------------------
-- === Lightweight 3D preview dock for creatures ===
local previewDock

function EnsurePreviewDock()
    if previewDock then return previewDock end
    previewDock = CreateFrame("Frame", "ATTGoGoPreviewDock", UIParent, BackdropTemplateMixin and "BackdropTemplate" or nil)
    previewDock:SetSize(260, 360)
    previewDock:SetFrameStrata("DIALOG")
    previewDock:SetFrameLevel(210)
    previewDock:SetClampedToScreen(true)
    previewDock:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background-Dark",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 }
    })
    previewDock:Hide()

    -- Creature model (no player gear, no TryOn)
    previewDock.model = CreateFrame("PlayerModel", nil, previewDock)
    previewDock.model:SetPoint("TOPLEFT", 6, -6)
    previewDock.model:SetPoint("BOTTOMRIGHT", -6, 6)

    -- gentle autorotation
    local rot = 0
    previewDock:SetScript("OnUpdate", function(self, elapsed)
        rot = (rot + elapsed * 0.6) % (2*math.pi)
        self.model:SetRotation(rot)
    end)

    return previewDock
end

local function EnsureBossItemsDock()
    if bossItemsDock then return bossItemsDock end
    bossItemsDock = CreateFrame("Frame", "ATTGoGoBossItemsDock", UIParent, BackdropTemplateMixin and "BackdropTemplate" or nil)
    bossItemsDock:SetSize(300, 360)
    bossItemsDock:SetFrameStrata("DIALOG")
    bossItemsDock:SetFrameLevel(211)
    bossItemsDock:SetClampedToScreen(true)
    bossItemsDock:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background-Dark",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 }
    })
    bossItemsDock.rows = {}
    bossItemsDock:Hide()

    bossItemsDock.title = bossItemsDock:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    bossItemsDock.title:SetPoint("TOPLEFT", 10, -10)
    bossItemsDock.title:SetPoint("TOPRIGHT", -10, -10)
    bossItemsDock.title:SetJustifyH("LEFT")
    bossItemsDock.title:SetText("Items")

    return bossItemsDock
end

local function ShowPreviewForNode(node)
    -- Only preview creatures on hover; items go to the Dressing Room via Ctrl+Click.
    if not (node and (node.creatureID or node.npcID)) or not GetSetting("showHover3DPreview", true) then
        previewDock:Hide()
        return
    end

    previewDock:ClearAllPoints()
    previewDock:SetPoint("TOPRIGHT",    uncollectedPopup, "TOPLEFT",   -8, 0)
    previewDock:SetPoint("BOTTOMRIGHT", uncollectedPopup, "BOTTOMLEFT", -8, 0)

    previewDock.model:SetCreature(node.creatureID or node.npcID)
    previewDock:Show()
end

-- List up to N dependent uncollected child collectibles on the tooltip (sub-achievements, item rewards, etc.)
local function AddUncollectedChildrenToTooltip(node)
    if type(node) ~= "table" or type(node.g) ~= "table" or next(node.g) == nil then return end
    local shown, extra = 0, 0
    for _, ch in pairs(node.g) do
        if type(ch) == "table" and ch.collectible and ch.collected ~= true then
            if shown < 21 then
                GameTooltip:AddLine("• " .. NodeShortName(ch), 1, 1, 1, true)
                shown = shown + 1
            else
                extra = extra + 1
            end
        end
    end
    if shown > 0 and extra > 0 then
        GameTooltip:AddLine(("And %d more..."):format(extra), 0.85, 0.85, 0.85, true)
    end
end

local function IsTooltipItemAllowed(node)
    if type(node) ~= "table" or not node.itemID or node.visible == false or node.collected then return false end
    if OPPOSITE_FACTION ~= 0 and node.r == OPPOSITE_FACTION then return false end
    if not INCLUDE_REMOVED and Util.IsNodeRemoved(node) then return false end

    local nc = node.c
    if nc ~= nil then
        if type(nc) == "table" then
            for i = 1, #nc do if nc[i] == CLASS_ID then return true end end
            return false
        end
        return nc == CLASS_ID
    end

    return true
end

local function IsDescendantOf(node, ancestor)
    local cur = rawget(node, "parent")
    while type(cur) == "table" do
        if cur == ancestor then return true end
        cur = rawget(cur, "parent")
    end
    return false
end

local function ListContainsID(list, id)
    if type(list) ~= "table" or not id then return false end
    for i = 1, #list do
        local v = list[i]
        if v == id then return true end
        if type(v) == "table" and (v[2] == id or v[1] == id) then return true end
    end
    return false
end

local function IsItemConnectedToBoss(itemNode, bossNode)
    if IsDescendantOf(itemNode, bossNode) then return true end

    local creatureID = bossNode.creatureID or bossNode.npcID
    if ListContainsID(itemNode.crs, creatureID) or ListContainsID(itemNode.providers, creatureID) then return true end
    if itemNode.creatureID == creatureID or itemNode.npcID == creatureID then return true end

    local sourceParent = rawget(itemNode, "sourceParent")
    return sourceParent == bossNode or (type(sourceParent) == "table" and (sourceParent.creatureID == creatureID or sourceParent.npcID == creatureID))
end

CollectBossItems = function(node)
    if type(node) ~= "table" or not (node.creatureID or node.npcID) then return end

    local out, seen = {}, {}
    local nodes = uncollectedPopup and uncollectedPopup.currentNodes
    if type(nodes) == "table" then
        for i = 1, #nodes do
            local child = nodes[i]
            local itemID = child and child.itemID
            if itemID and not seen[itemID] and IsTooltipItemAllowed(child) and IsItemConnectedToBoss(child, node) then
                seen[itemID] = true
                out[#out + 1] = child
            end
        end
    end
    return out
end

local CLASS_ALLOWED_ARMOR = {
    [1] = { plate = true }, -- Warrior
    [2] = { plate = true }, -- Paladin
    [3] = { mail = true },  -- Hunter
    [4] = { leather = true }, -- Rogue
    [5] = { cloth = true },  -- Priest
    [6] = { plate = true },  -- Death Knight
    [7] = { mail = true },   -- Shaman
    [8] = { cloth = true },  -- Mage
    [9] = { cloth = true },  -- Warlock
    [10] = { leather = true }, -- Monk
    [11] = { leather = true }, -- Druid
    [12] = { leather = true }, -- Demon Hunter
    [13] = { mail = true },  -- Evoker
}

local CLASS_ALLOWED_WEAPONS = {
    [1] = { axe = true, dagger = true, ["fist weapon"] = true, mace = true, polearm = true, sword = true, ["two-handed axe"] = true, ["two-handed mace"] = true, ["two-handed sword"] = true },
    [2] = { axe = true, dagger = true, mace = true, polearm = true, sword = true, ["two-handed axe"] = true, ["two-handed mace"] = true, ["two-handed sword"] = true },
    [3] = { axe = true, bow = true, crossbow = true, dagger = true, gun = true, ["fist weapon"] = true, mace = true, polearm = true, staff = true, sword = true, thrown = true, ["two-handed axe"] = true, ["two-handed mace"] = true, ["two-handed sword"] = true },
    [4] = { axe = true, dagger = true, ["fist weapon"] = true, mace = true, sword = true },
    [5] = { dagger = true, mace = true, wand = true, staff = true },
    [6] = { axe = true, dagger = true, ["fist weapon"] = true, mace = true, polearm = true, sword = true, ["two-handed axe"] = true, ["two-handed mace"] = true, ["two-handed sword"] = true },
    [7] = { axe = true, dagger = true, ["fist weapon"] = true, mace = true, shield = true, staff = true, ["two-handed axe"] = true, ["two-handed mace"] = true },
    [8] = { dagger = true, mace = true, wand = true, staff = true, sword = true },
    [9] = { dagger = true, wand = true, staff = true, sword = true },
    [10] = { dagger = true, ["fist weapon"] = true, mace = true, sword = true, staff = true },
    [11] = { dagger = true, ["fist weapon"] = true, mace = true, polearm = true, staff = true, sword = true, axe = true, ["two-handed axe"] = true, ["two-handed mace"] = true, ["two-handed sword"] = true },
    [12] = { axe = true, dagger = true, ["fist weapon"] = true, mace = true, polearm = true, sword = true, ["two-handed axe"] = true, ["two-handed mace"] = true, ["two-handed sword"] = true },
    [13] = { axe = true, dagger = true, ["fist weapon"] = true, mace = true, polearm = true, staff = true, sword = true, shield = true, ["two-handed axe"] = true, ["two-handed mace"] = true, ["two-handed sword"] = true },
}

local function NormalizeSubtype(subType)
    return subType and strlower(subType) or nil
end

local function IsArmorTypeAllowed(classID, subType)
    local allowed = CLASS_ALLOWED_ARMOR[classID]
    return allowed and allowed[NormalizeSubtype(subType)] or false
end

local function IsCollectibleArmorSubtype(subType)
    subType = NormalizeSubtype(subType)
    return subType == "cloth" or subType == "leather" or subType == "mail" or subType == "plate" or subType == "shield"
end

local function IsWeaponTypeAllowed(classID, subType)
    local allowed = CLASS_ALLOWED_WEAPONS[classID]
    return allowed and allowed[NormalizeSubtype(subType)] or false
end

CanShowItemForCharacter = function(item, scope)
    if not GetCharSetting("bossItemsForThisCharacterOnly", false) then return true end
    if not item or not item.itemID then return false end

    local bindPrefix = BossItemBindPrefix(item)
    if bindPrefix == "BoE " then
        return true
    end

    local _, itemType, itemSubType = GetItemInfoInstant(item.itemID)
    if itemType == "Armor" then
        if not IsCollectibleArmorSubtype(itemSubType) then
            return true
        end
        local ok = IsArmorTypeAllowed(CLASS_ID, itemSubType)
        return ok
    end
    if itemType == "Weapon" then
        return IsWeaponTypeAllowed(CLASS_ID, itemSubType)
    end
    if itemType == "Recipe" then
        local target = NormalizeSubtype(itemSubType or "")
        if target == "" then return true end
        local known = Util.GetKnownProfessions()
        for i = 1, #known do
            if NormalizeSubtype(known[i]) == target then return true end
        end
        return false
    end

    return true
end

local function ShowBossItemsForNode(node)
    if not (node and (node.creatureID or node.npcID)) then
        if bossItemsDock then bossItemsDock:Hide() end
        return
    end

    local dock = EnsureBossItemsDock()
    local items = CollectBossItems(node) or {}
    local visibleItems = {}
    for i = 1, #items do
        local item = items[i]
        if item and CanShowItemForCharacter(item, "boss") then
            visibleItems[#visibleItems + 1] = item
        end
    end

    dock:ClearAllPoints()
    if previewDock and previewDock:IsShown() then
        dock:SetPoint("TOPRIGHT", previewDock, "TOPLEFT", -8, 0)
        dock:SetPoint("BOTTOMRIGHT", previewDock, "BOTTOMLEFT", -8, 0)
    else
        dock:SetPoint("TOPRIGHT", uncollectedPopup, "TOPLEFT", -8, 0)
        dock:SetPoint("BOTTOMRIGHT", uncollectedPopup, "BOTTOMLEFT", -8, 0)
    end
    dock.title:SetText(SafeNodeName(node))

    local maxRows = 14
    for i = 1, maxRows do
        local row = dock.rows[i]
        if not row then
            row = dock:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
            row:SetPoint("TOPLEFT", 10, -16 - (i * ROW_HEIGHT))
            row:SetPoint("TOPRIGHT", -10, -16 - (i * ROW_HEIGHT))
            row:SetJustifyH("LEFT")
            row:SetWordWrap(false)
            dock.rows[i] = row
        end
        local item = visibleItems[i]
        if item then
            row:SetText(BossItemDisplayText(item))
            row:Show()
        else
            row:Hide()
        end
    end

    if not dock.moreLine then
        dock.moreLine = dock:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        dock.moreLine:SetPoint("BOTTOMLEFT", 10, 10)
        dock.moreLine:SetPoint("BOTTOMRIGHT", -10, 10)
        dock.moreLine:SetJustifyH("LEFT")
    end
    if #visibleItems > maxRows then
        dock.moreLine:SetText(("And %d more..."):format(#visibleItems - maxRows))
        dock.moreLine:Show()
    elseif #visibleItems == 0 then
        dock.moreLine:SetText("No matching uncollected items in this list.")
        dock.moreLine:Show()
    else
        dock.moreLine:Hide()
    end

    dock:Show()
end

-- Returns a single-line compact description of quest objectives, or nil if unavailable.
local function AddQuestObjectivesText(qid)
    local objs = C_QuestLog.GetQuestObjectives(qid)
    if not objs then GameTooltip:AddLine(Util.ATTSearchOne("questID", qid).name, 1, 1, 1, true); return false end
    for _, o in pairs(objs) do
        if o.text and o.text ~= "" then GameTooltip:AddLine(o.text, 1, 1, 1, true) end
    end
    return #objs > 0
end

-- Renders the quest tooltip once (no retry). Returns true if it printed real objectives.
local function RenderQuestTooltip(node)
    local hasLines = AddQuestObjectivesText(node.questID)
    AddUncollectedChildrenToTooltip(node)
    return hasLines
end

-- === World Map ping (brief highlight at coords) ===
local PingFrame
local function PingMapAt(mapID, x, y)
  if not (WorldMapFrame:IsShown() and WorldMapFrame:GetMapID() == mapID and x and y) then return end

  local child = WorldMapFrame.ScrollContainer
  if not PingFrame then
    PingFrame = CreateFrame("Frame", nil, child)
    PingFrame:SetSize(24, 24)
    local tex = PingFrame:CreateTexture(nil, "OVERLAY")
    tex:SetAllPoints()
    tex:SetTexture("Interface\\Buttons\\WHITE8X8")
    tex:SetVertexColor(1, 0, 0.2, 0.6)
    tex:SetMask("Interface\\CharacterFrame\\TempPortraitAlphaMask") -- makes it a circle
    PingFrame.tex = tex
    PingFrame:Hide()
  end

  local w, h = child:GetSize()
  PingFrame:ClearAllPoints()
  PingFrame:SetPoint("CENTER", child, "TOPLEFT", x * w, -y * h)
  PingFrame:Show()
  C_Timer.After(3.5, function() PingFrame:Hide() end)
end

local requestedOnce = {}

local function SetupNodeTooltip(btn)
    btn:SetScript("OnEnter", function(self)
        local node = self.node
        currentTooltipNode = node

        ShowPreviewForNode(node)
        ShowBossItemsForNode(node)

        -- brief attention ping on WorldMap near coords
        do
          local m,x,y = Util.ExtractMapAndCoords(node)
          if not m and node.instanceID then local inst = Util.ATTSearchOne("instanceID", node.instanceID); if inst then m,x,y=Util.ExtractMapAndCoords(inst) end end
          if not m and node.flightpathID and node.g then for i=1, #node.g do m, x, y = Util.ExtractMapAndCoords(node.g[i]); if m then break end end end
          if not m and node.parent then m, x, y = Util.ExtractMapAndCoords(node.parent) end
          PingMapAt(m, x, y)
        end

        if node and (node.creatureID or node.npcID) then
            return
        end

        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:ClearLines()
        local matched = passKeysByNode[node]
        if node.itemID then
            local id = node.itemID
            if GetItemInfo(id) then
                GameTooltip:SetItemByID(id)
            else
                GameTooltip:SetText(("Item %d"):format(id))
                if not requestedOnce[id] and not C_Item.IsItemDataCachedByID(id) then
                    requestedOnce[id] = true
                    C_Item.RequestLoadItemDataByID(id)
                end
            end
        elseif node.questID then
            local hadRealObjectives = RenderQuestTooltip(node)
            if not hadRealObjectives then
                C_Timer.After(0.50, function()
                    if currentTooltipNode == node and self:IsMouseOver() then
                        GameTooltip:ClearLines()
                        RenderQuestTooltip(node)
                        GameTooltip:Show()
                    end
                end)
            end
        elseif node.achievementID then
            GameTooltip:SetHyperlink(GetAchievementLink(node.achievementID))
            AddUncollectedChildrenToTooltip(node)
        else
            GameTooltip:AddLine(SafeNodeName(node), 1, 1, 1)
            AddUncollectedChildrenToTooltip(node)
        end
        GameTooltip:Show()
    end)
    btn:SetScript("OnLeave", function()
        currentTooltipNode = nil
        GameTooltip:Hide()
        if bossItemsDock then bossItemsDock:Hide() end
        previewDock:Hide()
    end)
end

------------------------------------------------------------
-- Lazy name resolution (items/achievements/spells)
------------------------------------------------------------
local hiddenTT
local primedItems = {}

local function PrimeItemInfo(itemID)
    if primedItems[itemID] then return end
    primedItems[itemID] = true

    hiddenTT = hiddenTT or CreateFrame("GameTooltip", "ATTGoGoHiddenTT", UIParent, "GameTooltipTemplate")
    hiddenTT:SetOwner(UIParent, "ANCHOR_NONE")
    hiddenTT:SetHyperlink("item:" .. itemID)
    hiddenTT:Hide()
end

------------------------------------------------------------
-- Display text
------------------------------------------------------------
local function ResolveDisplayForNode(node, label, btn)
    local display = RETRIEVING -- this is a very hot function, so don't pre-fetch `NodeShortName(node)` which is overwritten most of the time

    if node.itemID then
        local name, link = GetItemInfo(node.itemID)
        if link or name then
            display = link or NodeShortName(node) or name
            Util.ApplyNodeIcon(btn, node)
        else
            display = NodeShortName(node) or ("Item " .. node.itemID)
            itemLabelsByID[node.itemID] = { label = label, btn = btn }
            PrimeItemInfo(node.itemID)
        end
    elseif node.achievementID then
        local _, name = GetAchievementInfo(node.achievementID)
        display = name
        Util.ApplyNodeIcon(btn, node)
    elseif node.spellID then
        display = GetSpellInfo(node.spellID)
    elseif node.questID then
        local qid = node.questID
        local qname = (node.name and not IsPlaceholderTitle(node.name)) and node.name or C_QuestLog.GetQuestInfo(qid) or ("Quest " .. qid)
        display = qname
    else
        display = NodeShortName(node)
    end

    label:SetText(display)
end

------------------------------------------------------------
-- Data gathering (filter-aware) + sorting
------------------------------------------------------------
local CATEGORY_ORDER = { "titleID","achievementID","flightpathID","explorationID","instanceID","creatureID","mapID","itemID","questID" }
local CATEGORY_RANK = {}
for i, key in ipairs(CATEGORY_ORDER) do CATEGORY_RANK[key] = i end

local function GetNodePrimaryKey(node)
    --local matched = passKeysByNode and passKeysByNode[node]
    local matched = passKeysByNode[node]
    if matched and #matched > 0 then
        local bestKey, bestRank
        for _, k in ipairs(matched) do
            local r = CATEGORY_RANK[k]
            if r and (not bestRank or r < bestRank) then bestKey, bestRank = k, r end
        end
        if bestKey then return bestKey end
    end
    for _, k in ipairs(CATEGORY_ORDER) do
        if node[k] then return k end
    end
    TP(node, matched, #matched)
    return "zz_fallback"
end

-- De-duplicate achievements by achievementID, preferring a richer "meta" node over stubs.
local function DedupAchievements(nodes)
    if #nodes <= 1 then return nodes end

    local function richness(n)
        local r = 0
        if type(n) == "table" then
            if type(n.g) == "table" and next(n.g) ~= nil then r = r + 2 end
            if n.text or n.name then r = r + 1 end
            if n.icon then r = r + 1 end
        end
        return r
    end

    local uniq, byAch = {}, {}
    for i = 1, #nodes do
        local n = nodes[i]
        local aid = n and n.achievementID
        if aid then
            local prev = byAch[aid]
            if not prev then
                byAch[aid] = n
                uniq[#uniq+1] = n
            else
                -- Prefer the richer node (meta with children/label/icon) over a stub.
                if richness(n) > richness(prev) then
                    byAch[aid] = n
                    -- replace the previous entry in-place inside uniq
                    for j = 1, #uniq do
                        if uniq[j] == prev then
                            uniq[j] = n
                            break
                        end
                    end
                end
                -- else: keep prev, drop n
            end
        else
            uniq[#uniq+1] = n
        end
    end
    return uniq
end

-- Build active filter key list from current popup settings
local function CollectActiveKeys()
    local filters = ATTGoGoCharDB.popupIdFilters
    local activeKeys = {}
    for k, enabled in pairs(filters) do
        if enabled then activeKeys[#activeKeys+1] = k end
    end
    return activeKeys
end

-- Collapse repeated achievement criteria into the parent achievement (controlled by per-character option)
local function CollapseAchievementFamilies(root, nodes)
    local expandCriteria = GetCharSetting("expandAchievementCriteria", false)
    if expandCriteria or #nodes == 0 then return nodes end

    -- 1) find families present in the leaf list
    local families, keep = {}, {}
    for _, n in ipairs(nodes) do
        local aid = n.achID
        if aid then families[aid] = true else keep[#keep+1] = n end
    end

    if not next(families) then
        return nodes
    end

    -- 2) prefer real meta achievement nodes from the ATT tree
    local metas = {}
    local function scan_for_metas(t)
        if t.achievementID and not t.achID then
            metas[t.achievementID] = metas[t.achievementID] or t
        end
        local g = rawget(t, "g")
        for _, child in pairs(g or {}) do scan_for_metas(child) end
    end
    scan_for_metas(root)

    -- 3) add one representative per family unless completed
    for aid in pairs(families) do
        local rep = metas[aid] or { achievementID = aid }
        local _, _, _, completed = GetAchievementInfo(aid)
        if not completed then
            keep[#keep + 1] = rep
        end
    end

    -- 4) de-dup achievements (prefer richer)
    keep = DedupAchievements(keep)
    return keep
end

-- Map ATT/Item API qualities to a numeric rank (higher = better)
local function QualityRank(node)
    return node and node.q or 0
end

-- Group items by visualID, keeping the first item among the highest-quality tier
local function GroupItemsByVisualID(nodes)
    if #nodes <= 1 or not GetCharSetting("groupByVisualID", true) then return nodes end

    local keep, byVid = {}, {}
    for _, n in ipairs(nodes) do
        local vid = n.visualID
        if vid and n.itemID then
            local rec = byVid[vid]
            if not rec then
                local idx = #keep + 1
                keep[idx] = n
                byVid[vid] = { idx = idx, q = QualityRank(n) }
            else
                local q = QualityRank(n)
                if q > rec.q then
                    keep[rec.idx] = n
                    rec.q = q
                end
                -- same quality -> keep existing (deterministic "first of best")
            end
        else
            keep[#keep + 1] = n
        end
    end
    return keep
end

-- De-duplicate items by itemID, keeping only the first seen.
local function DedupItemsByItemID(nodes)
    if #nodes <= 1 then return nodes end
    local seen, keep = {}, {}
    for _, n in pairs(nodes) do
        local id = n.itemID
        if id then
            if not seen[id] then
                seen[id] = true
                keep[#keep + 1] = n
            end
            -- else: skip duplicate
        else
            keep[#keep + 1] = n
        end
    end
    return keep
end

-- Final sort used by the popup
local function SortPopupNodes(nodes)
    local function getID(n) return tonumber(n.itemID or n.achievementID or n.questID or n.mapID or n.instanceID or n.visualID or n.titleID or 0) end
    table.sort(nodes, function(a, b)
        local ak, bk = GetNodePrimaryKey(a), GetNodePrimaryKey(b)
        local ar, br = (CATEGORY_RANK[ak] or TP(ak) or 999), (CATEGORY_RANK[bk] or TP(bk) or 999)
        if ar ~= br then return ar < br end
        return getID(a) < getID(b)
    end)
end

local function GatherUncollectedNodes(node, out, keys)
    -- subtree fast-skip: if nothing uncollected lives here, don’t recurse
    local prog, total = Util.ATTGetProgress(node)
    if total and (total == 0 or prog >= total) then return end

    local isAllowed, matched = IsAllowedLeaf(node, keys)
    if isAllowed then
        out[#out + 1] = node
        passKeysByNode[node] = matched
    end

    local kids = node.g
    if type(kids) == "table" then
        for i = 1, #kids do
            local child = kids[i]
            if type(child) == "table" and child ~= node.parent then
                GatherUncollectedNodes(child, out, keys)
            end
        end
    end
end

-- Build + filter list
local function BuildNodeList(root)
    -- set hot-path locals for this traversal
    INCLUDE_REMOVED = GetSetting("includeRemoved", false)

    local perf_build_node_list = AGGPerf.auto("BuildNodeList")
    local activeKeys = CollectActiveKeys()
    if #activeKeys == 0 then return {}, activeKeys end

    ACTIVE_KEYS = activeKeys -- remember the keys for hot path in IsAllowedLeaf()

    local nodes = {}
    GatherUncollectedNodes(root, nodes, activeKeys)

    nodes = CollapseAchievementFamilies(root, nodes)
    nodes = DedupItemsByItemID(nodes)
    nodes = GroupItemsByVisualID(nodes)
    SortPopupNodes(nodes)

    perf_build_node_list()
    return nodes, activeKeys
end

------------------------------------------------------------
-- Row creation / rendering (virtualized)
------------------------------------------------------------
local function AcquireRow(scrollContent, i)
    scrollContent.rows = scrollContent.rows or {}
    local row = scrollContent.rows[i]
    if row then return row end

    __rowSerial = __rowSerial + 1
    local btnName = "ATTGoGoListItem" .. __rowSerial

    -- Create the button+label pair once
    local btn = CreateFrame("Button", btnName, scrollContent, "ItemButtonTemplate")
    btn:SetSize(ROW_BTN_SZ, ROW_BTN_SZ)

    -- hide "button" border art
    do
      local function hideBorder(tex) tex:SetTexture(nil); tex:SetAlpha(0); tex:Hide() end
      hideBorder(btn:GetNormalTexture()); hideBorder(btn:GetPushedTexture()); hideBorder(btn:GetHighlightTexture()); 
    end

    btn:RegisterForClicks("AnyUp")

    local label = scrollContent:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall") -- or "GameFontHighlightSmall" for a touch brighter
    label:SetWidth(220)
    label:SetJustifyH("LEFT")
    label:SetJustifyV("MIDDLE")

    -- Click + tooltip
    btn:SetScript("OnClick", function(self, mouseButton)
        local node = self.node

        -- Ctrl+click: open Dressing Room; undress first if option is ON
        if IsModifiedClick("DRESSUP") and node and node.itemID then
            local link = select(2, GetItemInfo(node.itemID)) or ("item:" .. node.itemID)

            ShowUIPanel(DressUpFrame)
            DressUpFrame.DressUpModel:SetUnit("player")
            if GetSetting("dressUpNaked", true) then DressUpFrame.DressUpModel:Undress() end
            DressUpFrame.DressUpModel:TryOn(link)
            return
        end

        if IsModifiedClick("CHATLINK") or mouseButton == "MiddleButton" then
            Util.InsertNodeChatLink(node)
            return
        end

        if mouseButton == "LeftButton" then
            -- 1) Direct achievement row
            if node.achievementID then
                Util.OpenAchievementByID(node.achievementID)
                return
            end
            -- 2) Title rows → resolve to the awarding achievement and open it
            if node.titleID then
                local aid = Util.FindAchievementForTitleNode(node)
                if aid then
                    Util.OpenAchievementByID(aid)
                    return
                end
            end
            -- 3) POI rows that can focus the map
            if node.mapID or node.explorationID or node.instanceID or node.flightpathID or node.questID or node.creatureID or node.npcID or node.itemID then
                Util.FocusMapForNode(node)
            end
        end

        if mouseButton == "RightButton" then
            if IsAltKeyDown() then
                Util.FocusMapForNode(node)
            end
        end
    end)
    SetupNodeTooltip(btn)

    row = { btn = btn, label = label }
    scrollContent.rows[i] = row
    return row
end

local function RenderRowAt(scrollContent, row, dataIndex, nodes)
    local node = nodes[dataIndex]
    if not node then
        row.btn:Hide()
        row.label:Hide()
        row.btn.node = nil
        return
    end

    -- Absolute placement inside the scrolled content
    local y = -((dataIndex - 1) * ROW_HEIGHT) + 1
    row.btn:ClearAllPoints()
    row.btn:SetPoint("TOPLEFT", 5, y)
    row.label:ClearAllPoints()
    row.label:SetPoint("LEFT", row.btn, "RIGHT", 6, 0)

    -- Fill visuals (fast path: icon + name)
    row.btn.node = node
    Util.ApplyNodeIcon(row.btn, node)
    ResolveDisplayForNode(node, row.label, row.btn)

    row.btn:Show()
    row.label:Show()
end

local function UpdateVirtualList()
    local nodes = uncollectedPopup.currentNodes or {}
    local scroller = uncollectedPopup.scrollFrame
    local content  = uncollectedPopup.scrollContent

    -- viewport
    local viewH = math.max(uncollectedPopup:GetHeight() - 45, ROW_HEIGHT)
    local first   = math.floor(scroller:GetVerticalScroll() / ROW_HEIGHT) + 1
    local visible = math.ceil(viewH / ROW_HEIGHT) + ROW_BUFFER

    -- ensure rows
    for i = 1, visible do
        local row = AcquireRow(content, i)
        RenderRowAt(content, row, first + (i - 1), nodes)
    end
end

------------------------------------------------------------
-- Popup UI creation and persistence
------------------------------------------------------------
function EnsurePopup()
    if uncollectedPopup then return end

    uncollectedPopup = CreateFrame("Frame", "ATTGoGoUncollectedPopup", UIParent, BackdropTemplateMixin and "BackdropTemplate" or nil)
    uncollectedPopup:SetSize(268, 592)
    uncollectedPopup:SetClampedToScreen(true)
    uncollectedPopup:SetResizeBounds(180, 120, 800, 800)
    uncollectedPopup:SetResizable(true)
    Util.EnableDragPersist(uncollectedPopup, "popupWindowPos")

    -- look & strata
    uncollectedPopup:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 }
    })
    uncollectedPopup:SetFrameStrata("MEDIUM")
    table.insert(UISpecialFrames, "ATTGoGoUncollectedPopup")

    -- title + close
    uncollectedPopup.title = uncollectedPopup:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    uncollectedPopup.title:SetPoint("TOPLEFT", 12, -10)
    uncollectedPopup.title:SetPoint("TOPRIGHT", -24, -10)
    uncollectedPopup.title:SetWordWrap(true)
    uncollectedPopup.title:SetNonSpaceWrap(false)
    uncollectedPopup.title:SetText("Missing Items")

    local closeBtn = CreateFrame("Button", nil, uncollectedPopup, "UIPanelCloseButton")
    closeBtn:SetPoint("TOPRIGHT", 0, 0)
    closeBtn:SetScript("OnClick", function() uncollectedPopup:Hide() end)

    -- scroll frame + content
    local scroll = CreateFrame("ScrollFrame", nil, uncollectedPopup, "UIPanelScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", 10, -35)
    scroll:SetPoint("BOTTOMRIGHT", -30, 10)
    local content = CreateFrame("Frame", nil, scroll)
    content:SetSize(1, 1)
    scroll:SetScrollChild(content)

    uncollectedPopup.scrollFrame   = scroll
    uncollectedPopup.scrollContent = content

    Util.EnableScrollDrag(uncollectedPopup.scrollFrame, uncollectedPopup, "popupWindowPos")

    -- scrollbar sync
    scroll:SetScript("OnScrollRangeChanged", function(self, _, yRange)
        local max = math.max(yRange or 0, 0)
        self.ScrollBar:SetMinMaxValues(0, max)
        if self.ScrollBar:GetValue() > max then
            self.ScrollBar:SetValue(max)
        end
    end)
    scroll:SetScript("OnVerticalScroll", function(self, offset)
        UpdateVirtualList()
    end)
    scroll:EnableMouseWheel(true)
    scroll:SetScript("OnMouseWheel", function(self, delta)
        local step = ROW_HEIGHT * 3
        local current = self:GetVerticalScroll()
        local _, m = self.ScrollBar:GetMinMaxValues()
        local newOffset = math.max(0, math.min(current - delta * step, m))
        self.ScrollBar:SetValue(newOffset)
    end)
    scroll.ScrollBar:SetScript("OnValueChanged", function(sb, value)
        scroll:SetVerticalScroll(value or 0)
        UpdateVirtualList()
    end)

    Util.AddResizerCorner(uncollectedPopup, "popupWindowPos", UpdateVirtualList)

    Util.PersistOnSizeChanged(uncollectedPopup, "popupWindowPos", function()
        uncollectedPopup.scrollFrame:UpdateScrollChildRect()
        UpdateVirtualList()
    end)

    uncollectedPopup:SetScript("OnHide", function(self)
        previewDock:Hide()
        if bossItemsDock then bossItemsDock:Hide() end
        Util.SaveFramePosition(self, "popupWindowPos")
    end)

    -- finally, restore last position/size
    Util.LoadFramePosition(uncollectedPopup, "popupWindowPos", "RIGHT", -200, 64)

    uncollectedPopup:Hide()

end

------------------------------------------------------------
-- Populate & refresh (virtualized)
local function PopulateUncollectedPopup(scrollContent, nodes)
    -- Adjust content height / empty state
    if #nodes == 0 then
        scrollContent.emptyLine = scrollContent.emptyLine or scrollContent:CreateFontString(nil, "OVERLAY", "GameFontNormal")
        local line = scrollContent.emptyLine
        line:ClearAllPoints()
        line:SetPoint("TOPLEFT", 5, 0)
        line:SetText("All collected!")
        line:Show()
        scrollContent:SetHeight(40)
    else
        if scrollContent.emptyLine then scrollContent.emptyLine:Hide() end
        scrollContent:SetHeight(#nodes * ROW_HEIGHT + 8)
    end

    -- Preserve current scroll offset
    local scroll = scrollContent:GetParent()
    local prevOffset = scroll:GetVerticalScroll()
    scroll:UpdateScrollChildRect()

    local _, m = scroll.ScrollBar:GetMinMaxValues()
    prevOffset = math.max(0, math.min(prevOffset, m))
    scroll.ScrollBar:SetValue(prevOffset)

    UpdateVirtualList()
end

------------------------------------------------------------
-- Data-updater frame (late item/spell names)
------------------------------------------------------------
local updater = CreateFrame("Frame")
updater:RegisterEvent("GET_ITEM_INFO_RECEIVED")
updater:RegisterEvent("SPELLS_CHANGED")
updater:RegisterEvent("ITEM_DATA_LOAD_RESULT")

updater:SetScript("OnEvent", function(_, event, ...)
    local function SetItemLabel(itemID)
        local entry = itemLabelsByID[itemID]
        if not entry then return end
        local name, link = GetItemInfo(itemID)
        if link or name then
            entry.label:SetText(link or name)
            Util.ApplyNodeIcon(entry.btn, entry.btn.node)
            itemLabelsByID[itemID] = nil
            requestedOnce[itemID] = nil
        end
    end
    if event == "GET_ITEM_INFO_RECEIVED" or event == "ITEM_DATA_LOAD_RESULT" then
        local itemID, ok = ...
        if ok then SetItemLabel(itemID) end
    elseif event == "SPELLS_CHANGED" then
        for id, label in pairs(spellLabelsByID) do
            local name = GetSpellInfo(id)
            if name then label:SetText(name); spellLabelsByID[id] = nil end
        end
    end
end)

------------------------------------------------------------
-- Build + show
------------------------------------------------------------
local function RefreshPopup(data)
    uncollectedPopup.currentData = data

    local nodes, activeKeys = BuildNodeList(data)
    uncollectedPopup.currentNodes = nodes
    PopulateUncollectedPopup(uncollectedPopup.scrollContent, nodes)

    uncollectedPopup.title:SetText(("%s (%d)"):format(Util.NodeDisplayName(data), #nodes))
end

function ShowUncollectedPopup(data)
    RefreshPopup(data)
    uncollectedPopup:Show()
end
