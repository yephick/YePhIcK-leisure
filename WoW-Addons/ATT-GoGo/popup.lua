-- popup.lua
-- Virtualized uncollected-list popup: fast, low-GC, and flicker-free.

------------------------------------------------------------
-- Module locals / state
------------------------------------------------------------
local uncollectedPopup          -- Frame (name: ATTGoGoUncollectedPopup)
local currentTooltipNode        -- last node whose tooltip is active

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

local function IsAllowedLeaf(node, activeKeys)
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

    -- Removed gate (setting once, no per-node GetSetting)
    if not INCLUDE_REMOVED and Util.IsNodeRemoved(node) then
        return false, nil
    end

    -- visibility/uncollected flags
    if node.visible == false or node.collected then
        return false, nil
    end

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

------------------------------------------------------------
-- Tooltip helpers
------------------------------------------------------------
local function CollectIdFields(node)
    local keys = {}
    for k, v in pairs(node) do
        if v ~= nil and v ~= "" and type(k) == "string" and k:find("ID", 1, true) then
            keys[#keys + 1] = k
        end
    end
    return keys
end

local function AddMatchedIDLines(node)
    local keys = CollectIdFields(node) or print(CTITLE .. "trying node.parent") or node.parent and CollectIdFields(node.parent)
    table.sort(keys)
    if #keys == 0 then return end

    GameTooltip:AddLine(" ")
    for _, k in ipairs(keys) do
        local v = node[k]
        GameTooltip:AddLine(k .. ": " .. v, 1, 1, 1)
    end
end

-- One-time hook to re-append our lines whenever the item tooltip is rebuilt. N.B.: items in bags (for instance) don't have/need our tooltip hook
if not GameTooltip.__ATTGoGoHooked then
    GameTooltip:HookScript("OnTooltipSetItem", function(tt)
        if currentTooltipNode then
            AddMatchedIDLines(currentTooltipNode)
        end
    end)

    -- Quests/objects/spells/etc. that arrive via SetHyperlink (these often rebuild a tick later)
    hooksecurefunc(GameTooltip, "SetHyperlink", function(tt, link)
        if tt:IsShown() and currentTooltipNode then
            AddMatchedIDLines(currentTooltipNode)
        end
    end)

    GameTooltip.__ATTGoGoHooked = true
end

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

local function ShowPreviewForNode(node)
    -- Only preview creatures on hover; items go to the Dressing Room via Ctrl+Click.
    if not (node and (node.creatureID or node.npcID)) or not GetSetting("showHover3DPreview", true) then
        previewDock:Hide(); return
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

        -- brief attention ping on WorldMap near coords
        do
          local m,x,y = Util.ExtractMapAndCoords(node)
          if not m and node.instanceID then local inst = Util.ATTSearchOne("instanceID", node.instanceID); if inst then m,x,y=Util.ExtractMapAndCoords(inst) end end
          if not m and node.flightpathID and node.g then for i=1, #node.g do m, x, y = Util.ExtractMapAndCoords(node.g[i]); if m then break end end end
          if not m and node.parent then m, x, y = Util.ExtractMapAndCoords(node.parent) end
          PingMapAt(m, x, y)
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
            local aID = node.achievementID
            local link = GetAchievementLink(aID)
            if link then
                GameTooltip:SetHyperlink(link)
            else
                TP(aID)
                local _, aName = GetAchievementInfo(aID)
                GameTooltip:AddLine(aName or ("Achievement " .. aID), 1, 1, 1, true)
            end

            AddUncollectedChildrenToTooltip(node)
        elseif node.creatureID or node.npcID then
            GameTooltip:AddLine(SafeNodeName(node), 1, 1, 1)
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

local retryTicker, retryCount
local function EnsureRetryTicker()
    if retryTicker then return end
    retryCount = 0
    retryTicker = C_Timer.NewTicker(0.5, function()
    TP(retryCount, achLabelsByID,spellLabelsByID)
    AGGPerf.wrap("EnsureRetryTicker:retryTicker", function()
        retryCount = retryCount + 1
        for id, label in pairs(achLabelsByID) do
            local _, name = GetAchievementInfo(id)
            if name and name ~= "" then label:SetText(name); achLabelsByID[id] = nil end
        end
        for id, label in pairs(spellLabelsByID) do
            local name = GetSpellInfo(id)
            if name then label:SetText(name); spellLabelsByID[id] = nil else TP(id, label, name) end
        end
        if (not next(achLabelsByID)) and (not next(spellLabelsByID)) or retryCount >= 20 then
            retryTicker:Cancel(); retryTicker = nil
        end
    end)
    end)
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
        if name and name ~= "" then
            display = name
            Util.ApplyNodeIcon(btn, node)
        else
            display = NodeShortName(node) or ("Achievement " .. node.achievementID)
            achLabelsByID[node.achievementID] = label
            EnsureRetryTicker()
        end
    elseif node.spellID then
        local link = GetSpellInfo(node.spellID)
        if link then
            display = link
        else
            display = NodeShortName(node) or ("Spell " .. node.spellID)
            spellLabelsByID[node.spellID] = label
            EnsureRetryTicker()
        end
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
local CATEGORY_ORDER = {
    "titleID","achievementID","flightpathID","explorationID","instanceID",
    "visualID","creatureID","mapID","itemID","questID",
}
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
        if type(t) ~= "table" then TP(t); return end
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

local SKIP_FULLY_COLLECTED = true    -- feature flag (toggle for A/B)
local VISITS, EMITS, SKIPS = 0, 0, 0 -- lightweight visit stats

local function GatherUncollectedNodes(node, out, keys, seen)--, d)
--local depth = (d or 0) + 1
--local site = "GatherUncollectedNodes:" .. (d or 0)
--AGGPerf.wrap(site, function()
    if type(node) ~= "table" then TP(node); return end

    seen = seen or setmetatable({}, { __mode = "k" })
    if seen[node] then TP(seen[node]); return end
    seen[node] = true

    VISITS = VISITS + 1

    -- subtree fast-skip: if nothing uncollected lives here, don’t recurse
    if SKIP_FULLY_COLLECTED then
        local prog, total = Util.ATTGetProgress(node)
        -- skip when container is obviously empty or fully done
        if total and (total == 0 or prog == total) then
            SKIPS = SKIPS + 1
            return
        end
    end

    local isAllowed, matched = IsAllowedLeaf(node, keys)
    if isAllowed then
        EMITS = EMITS + 1
        out[#out + 1] = node
        passKeysByNode[node] = matched
    end

    local kids = node.g
    if type(kids) == "table" then
--    local recursion = AGGPerf.auto(site .. ":recursion with " .. #kids .. " children")
        for i = 1, #kids do
            local child = kids[i]
            if type(child) == "table" and child ~= node.parent then
                GatherUncollectedNodes(child, out, keys, seen)--, depth)
            end
        end
--    recursion()
    end
--end)
end

-- Build + filter list
local function BuildNodeList(root)
return AGGPerf.wrap("BuildNodeList", function()
    local activeKeys = CollectActiveKeys()
    if #activeKeys == 0 then return {}, activeKeys end

    -- set hot-path locals for this traversal
    ACTIVE_KEYS = activeKeys
    INCLUDE_REMOVED = GetSetting("includeRemoved", false)

    -- Gather raw leaves per active filters
    local nodes = {}
--    AGGPerf.wrap("BuildNodeList:GatherUncollectedNodes", function()
        VISITS, EMITS, SKIPS = 0, 0, 0
        GatherUncollectedNodes(root, nodes, activeKeys)
--    end)

    -- one-line summary
    DebugLogf("GatherUncollectedNodes:stats visits=%d emits=%d skips=%d keys=%d skip_full=%s", VISITS, EMITS, SKIPS, #ACTIVE_KEYS, tostring(SKIP_FULLY_COLLECTED))

    -- transformations
    nodes = CollapseAchievementFamilies(root, nodes)
    nodes = DedupItemsByItemID(nodes)
    nodes = GroupItemsByVisualID(nodes)
    SortPopupNodes(nodes)

    return nodes, activeKeys
end)
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
      local t = btn:GetNormalTexture(); t:SetTexture(nil); t:SetAlpha(0); t:Hide()
      local p = btn:GetPushedTexture(); p:SetTexture(nil); p:SetAlpha(0); p:Hide()
      local h = btn:GetHighlightTexture(); h:SetTexture(nil); h:SetAlpha(0); h:Hide()
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
                if Util.FocusMapForNode(node) then return end
            end
        end

        if mouseButton == "RightButton" and IsAltKeyDown() then
            Util.FocusMapForNode(node)
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
            if entry.btn and entry.btn.node then Util.ApplyNodeIcon(entry.btn, entry.btn.node) else TP() end
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
