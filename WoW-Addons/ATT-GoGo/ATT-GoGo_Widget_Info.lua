-- ATT-GoGo_Widget_Info.lua
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
local ROW_HEIGHT = 36           -- row height
local ROW_BUFFER = 6            -- render-ahead buffer
local __rowSerial = 0           -- unique names for ItemButtonTemplate rows

-- Returns (isAllowed, matchedKeys[])
local function IsAllowedLeaf(node, activeKeys)
    if type(node) ~= "table" then return false end

    local includeRemoved = GetSetting("includeRemoved", false)
    if not includeRemoved then
        if Util.IsNodeRemoved(node) then
            return false, {}   -- filtered out as 'removed'
        end
    end

    local matched = {}
    if type(activeKeys) == "table" and #activeKeys > 0 then
        -- respect current filter selection
        for i = 1, #activeKeys do
            local k = activeKeys[i]
            local v = node[k]
            if v ~= nil and v ~= 0 then matched[#matched + 1] = k end
        end
    else
        -- fallback: use global defaults if no active keys provided
        for k, _ in pairs(COLLECTIBLE_ID_FIELDS) do
            local v = node[k]
            if v ~= nil and v ~= 0 then matched[#matched + 1] = k end
        end
    end

    local isVisible     = (node.visible ~= false)
    local isUncollected = (node.collected ~= true)

    if isUncollected and isVisible and #matched > 0 then
        return true, matched
    end
    return false, matched
end

local function SafeNodeName(n)
    if not n or type(n) ~= "table" then return "?" end
    return n.text or n.name or _G.UNKNOWN or "?"
end

-- Short display name for a collectible leaf
local function NodeShortName(n)
    if not n or type(n) ~= "table" then return "Collectible" end
    if n.text and n.text ~= "" then return n.text end
    if n.name and n.name ~= "" then return n.name end
    if n.itemID and GetItemInfo then
        local nm = GetItemInfo(n.itemID); if nm then return nm end
        return "Item " .. tostring(n.itemID)
    end
    if n.achievementID then
        local _, nm = GetAchievementInfo(n.achievementID); if nm and nm ~= "" then return nm end
        return "Achievement " .. tostring(n.achievementID)
    end
    if n.spellID then
        local nm = GetSpellInfo(n.spellID); if nm then return nm end
        return "Spell " .. tostring(n.spellID)
    end
    if n.questID then return "Quest " .. tostring(n.questID) end
    if n.titleID then return "Title " .. tostring(n.titleID) end
    return "Collectible"
end

------------------------------------------------------------
-- Tooltip helpers
------------------------------------------------------------
local function AddMatchedIDLines(node, matchedKeys)
    if not matchedKeys or #matchedKeys == 0 then return false end
    GameTooltip:AddLine(" ")
    for _, k in ipairs(matchedKeys) do
        local v = node[k]
        if v and v ~= 0 then
            local label = COLLECTIBLE_ID_LABELS[k] or k
            GameTooltip:AddLine(label .. " ID: " .. tostring(v), 1, 1, 1)
        end
    end
    return true
end

-- One-time hook to re-append our lines whenever the item tooltip is rebuilt
if not GameTooltip.__ATTGoGoHooked then
    GameTooltip:HookScript("OnTooltipSetItem", function(tt)
        if currentTooltipNode and not tt.__ATTGoGoReentrant then
            tt.__ATTGoGoReentrant = true
            local matched = passKeysByNode[currentTooltipNode]
            AddMatchedIDLines(currentTooltipNode, matched)
            tt.__ATTGoGoReentrant = nil
        end
    end)
    GameTooltip.__ATTGoGoHooked = true
end

-- === Lightweight 3D preview dock for creatures ===
local previewDock

local function EnsurePreviewDock()
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
        if self.model and self.model.SetRotation then self.model:SetRotation(rot) end
    end)

    return previewDock
end

local function HidePreview()
    if previewDock then previewDock:Hide() end
end

local function ShowPreviewForNode(node)
    -- Only preview creatures on hover; items go to the Dressing Room via Ctrl+Click.
    if not GetSetting("showHover3DPreview", true) then
        HidePreview(); return
    end
    if not (uncollectedPopup and uncollectedPopup:IsShown()) then
        return
    end
    if not (node and node.creatureID) then
        HidePreview(); return
    end

    local dock = EnsurePreviewDock()
    dock:ClearAllPoints()
    dock:SetPoint("TOPRIGHT",    uncollectedPopup, "TOPLEFT",   -8, 0)
    dock:SetPoint("BOTTOMRIGHT", uncollectedPopup, "BOTTOMLEFT", -8, 0)

    local mdl = dock.model
    if not mdl then return end
    pcall(mdl.SetCreature, mdl, node.creatureID)
    dock:Show()
end

local function SetupNodeTooltip(btn, boundNode)
    btn:SetScript("OnEnter", function(self)
        local node = self.node or boundNode
        if not node then return end
        currentTooltipNode = node

        if node.creatureID then
            ShowPreviewForNode(node)
        else
            HidePreview()
        end

        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:ClearLines()
        local matched = passKeysByNode[node]
        if node.itemID then
            GameTooltip:SetHyperlink("item:" .. node.itemID)
        elseif node.questID then
            local qid = node.questID
            local parts = {}
            if C_QuestLog and C_QuestLog.GetQuestObjectives then
                local objs = C_QuestLog.GetQuestObjectives(qid)
                if type(objs) == "table" then
                    for i = 1, #objs do
                        local o = objs[i]
                        if o and o.text and o.text ~= "" then parts[#parts+1] = o.text end
                    end
                end
            end
            local shortDesc
            if #parts > 0 then
                shortDesc = table.concat(parts, " "):gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
            end
            GameTooltip:AddLine(shortDesc or "Objective unavailable", 1,1,1, true)
            AddMatchedIDLines(node, matched)

            if not shortDesc then
                if C_QuestLog and C_QuestLog.RequestLoadQuestByID then
                    pcall(C_QuestLog.RequestLoadQuestByID, qid)
                end
                local owner = self
                C_Timer.After(0.50, function()
                    if currentTooltipNode == node and owner:IsMouseOver() then
                        GameTooltip:ClearLines()
                        local objs = C_QuestLog and C_QuestLog.GetQuestObjectives and C_QuestLog.GetQuestObjectives(qid)
                        local p2 = {}
                        if type(objs) == "table" then
                            for i = 1, #objs do
                                local o = objs[i]; if o and o.text and o.text ~= "" then p2[#p2+1] = o.text end
                            end
                        end
                        local s2 = (#p2 > 0) and (table.concat(p2, " "):gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")) or "Objective unavailable"
                        GameTooltip:AddLine(s2, 1,1,1, true)
                        AddMatchedIDLines(node, passKeysByNode[node])
                        GameTooltip:Show()
                    end
                end)
            end
        elseif node.achievementID then
            local aID = node.achievementID
            local _, aName = GetAchievementInfo(aID)
            GameTooltip:AddLine(aName or ("Achievement " .. tostring(aID)), 1, 1, 1, true)

            local any = false
            local num = GetAchievementNumCriteria(aID) or 0
            for i = 1, num do
                local cName, _, cDone = GetAchievementCriteriaInfo(aID, i)
                if not cDone and cName and cName ~= "" then
                    GameTooltip:AddLine("• " .. cName, 1, 1, 1, true)
                    any = true
                end
            end
            AddMatchedIDLines(node, matched)
            GameTooltip:Show()
        elseif node.creatureID then
            GameTooltip:AddLine(SafeNodeName(node), 1, 1, 1)

            -- List up to 7 uncollected collectibles obtainable from this creature
            if type(node.g) == "table" and #node.g > 0 then
                local shown, extra = 0, 0
                for i = 1, #node.g do
                    local ch = node.g[i]
                    if type(ch) == "table" and ch.collectible and ch.collected ~= true then
                        if shown < 7 then
                            GameTooltip:AddLine("• " .. NodeShortName(ch), 1, 1, 1, true)
                            shown = shown + 1
                        else
                            extra = extra + 1
                        end
                    end
                end
                if shown > 0 and extra > 0 then
                    GameTooltip:AddLine(string.format("And %d more...", extra), 0.85, 0.85, 0.85, true)
                end
            end

            AddMatchedIDLines(node, matched)
        else
            GameTooltip:AddLine(SafeNodeName(node), 1, 1, 1)
            AddMatchedIDLines(node, matched)
        end
        GameTooltip:Show()
    end)
    btn:SetScript("OnLeave", function()
        currentTooltipNode = nil
        GameTooltip:Hide()
        HidePreview()
    end)
end

------------------------------------------------------------
-- Lazy name resolution (items/achievements/spells)
------------------------------------------------------------
local hiddenTT
local function PrimeItemInfo(itemID)
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
        retryCount = retryCount + 1
        for id, label in pairs(achLabelsByID) do
            local _, name = GetAchievementInfo(id)
            if name and name ~= "" then label:SetText(name); achLabelsByID[id] = nil end
        end
        for id, label in pairs(spellLabelsByID) do
            local name = GetSpellInfo(id)
            if name then label:SetText(name); spellLabelsByID[id] = nil end
        end
        if (not next(achLabelsByID)) and (not next(spellLabelsByID)) or retryCount >= 20 then
            retryTicker:Cancel(); retryTicker = nil
        end
    end)
end

------------------------------------------------------------
-- Display text
------------------------------------------------------------
local RETRIEVING = "Retrieving data"
local function IsPlaceholderTitle(t)
    return (not t) or t == "" or t == RETRIEVING or (t and t:lower():find("retrieving"))
end

local function ResolveDisplayForNode(node, label, btn)
    local display = node.text or node.name

    if node.itemID then
        local name = GetItemInfo(node.itemID)
        if name then
            display = display or name
            Util.ApplyNodeIcon(btn, node)
        else
            display = display or ("Item " .. tostring(node.itemID))
            itemLabelsByID[node.itemID] = { label = label, btn = btn }
            PrimeItemInfo(node.itemID)
        end
    elseif node.achievementID then
        local _, name = GetAchievementInfo(node.achievementID)
        if name and name ~= "" then
            display = display or name
            Util.ApplyNodeIcon(btn, node)
        else
            display = display or ("Achievement " .. tostring(node.achievementID))
            achLabelsByID[node.achievementID] = label
            EnsureRetryTicker()
        end
    elseif node.spellID then
        local name = GetSpellInfo(node.spellID)
        if name then
            display = display or name
        else
            display = display or ("Spell " .. tostring(node.spellID))
            spellLabelsByID[node.spellID] = label
            EnsureRetryTicker()
        end
    elseif node.questID then
        local qid = node.questID
        local qname = (node.name and not IsPlaceholderTitle(node.name)) and node.name or ("Quest " .. tostring(qid))
        display = qname
    end

    label:SetText(display or "Collectible")
end

------------------------------------------------------------
-- Data gathering (filter-aware) + sorting
------------------------------------------------------------
local CATEGORY_ORDER = {
    "titleID","achievementID","flightpathID","explorationID","instanceID",
    "questID","visualID","creatureID","mapID","itemID","gearSetID",
}
local CATEGORY_RANK = {}
for i, key in ipairs(CATEGORY_ORDER) do CATEGORY_RANK[key] = i end

local function GetNodePrimaryKey(node)
    local matched = passKeysByNode and passKeysByNode[node]
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
    return "zz_fallback"
end

local function GetNodeDisplayName(node)
    local display = node.text or node.name
    if display and display ~= "" then return display:lower() end
    if node.itemID and GetItemInfo then
        local name = GetItemInfo(node.itemID); if name then return name:lower() end
        return ("item %d"):format(node.itemID)
    end
    if node.achievementID and GetAchievementInfo then
        local _, name = GetAchievementInfo(node.achievementID)
        if name and name ~= "" then return name:lower() end
        return ("achievement %d"):format(node.achievementID)
    end
    if node.questID then return ("quest %d"):format(node.questID) end
    if node.mapID then return ("map %d"):format(node.mapID) end
    if node.instanceID then return ("instance %d"):format(node.instanceID) end
    if node.visualID then return ("visual %d"):format(node.visualID) end
    if node.gearSetID then return ("gear set %d"):format(node.gearSetID) end
    if node.flightpathID then return ("flight path %d"):format(node.flightpathID) end
    if node.explorationID then return ("exploration %d"):format(node.explorationID) end
    if node.titleID then return ("title %d"):format(node.titleID) end
    return "zzz"
end

local function QuestDedupKey(qid, node)
    local title = node and node.name
    if title and not IsPlaceholderTitle(title) then
        return "title:" .. title:lower()
    else
        return "qid:" .. tostring(qid)
    end
end

-- De-duplicate achievements by achievementID, preferring a richer "meta" node over stubs.
local function DedupAchievements(nodes)
    if type(nodes) ~= "table" or #nodes <= 1 then return nodes end

    local function richness(n)
        local r = 0
        if type(n) == "table" then
            if type(n.g) == "table" and #n.g > 0 then r = r + 2 end
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
    local filters = Util.GetPopupIdFilters()
    local activeKeys = {}
    for k, enabled in pairs(filters or {}) do
        if enabled then activeKeys[#activeKeys+1] = k
        end
    end
    return activeKeys
end

-- Quest de-duplication by stable title-or-qid key, keeping the highest qid when titles match
local function DedupQuests(nodes)
    if type(nodes) ~= "table" or #nodes <= 1 then return nodes end
    local keep, byKey = {}, {}
    for _, n in ipairs(nodes) do
        if n.questID then
            local key = QuestDedupKey(n.questID, n)
            local prev = byKey[key]
            if (not prev) or (tonumber(n.questID) or 0) > (tonumber(prev.questID) or 0) then
                byKey[key] = n
            end
        else
            keep[#keep+1] = n
        end
    end
    for _, n in pairs(byKey) do keep[#keep+1] = n end
    return keep
end

-- Collapse repeated achievement criteria into the parent achievement (controlled by per-character option)
local function CollapseAchievementFamilies(root, nodes)
    local expandCriteria = GetCharSetting("expandAchievementCriteria", false)
    if expandCriteria or type(nodes) ~= "table" or #nodes == 0 then
        return nodes
    end

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
        if type(t) ~= "table" then return end
        if t.achievementID and not t.achID then
            metas[t.achievementID] = metas[t.achievementID] or t
        end
        local g = rawget(t, "g")
        if type(g) == "table" then
            for i = 1, #g do scan_for_metas(g[i]) end
        end
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
    -- Prefer ATT's 'q' (already numeric, 0..7). Fallback to GetItemInfo.
    local q = tonumber(node and node.q)
    if not q and node and node.itemID and GetItemInfo then
        q = select(3, GetItemInfo(node.itemID))
    end
    q = tonumber(q) or 0
    return q
end

-- Group items by visualID, keeping the first item among the highest-quality tier
local function GroupItemsByVisualID(nodes)
    local groupVisuals = GetCharSetting("groupByVisualID", true)
    if (not groupVisuals) or type(nodes) ~= "table" or #nodes <= 1 then
        return nodes
    end

    local keep, byVid = {}, {}
    for _, n in ipairs(nodes) do
        local vid = n and n.visualID
        if vid and n.itemID then
            local g = byVid[vid]
            if not g then
                byVid[vid] = n
                keep[#keep + 1] = n
            else
                local qNew, qOld = QualityRank(n), QualityRank(g)
                if qNew > qOld then
                    byVid[vid] = n
                    for i = 1, #keep do
                        if keep[i] == g then keep[i] = n; break end
                    end
                end
                -- same quality -> keep existing (deterministic "first of best")
            end
        else
            keep[#keep + 1] = n
        end
    end
    return keep
end

-- Final sort used by the popup
local function SortPopupNodes(nodes)
    table.sort(nodes, function(a, b)
        local ak, bk = GetNodePrimaryKey(a), GetNodePrimaryKey(b)
        local ar, br = (CATEGORY_RANK[ak] or 999), (CATEGORY_RANK[bk] or 999)
        if ar ~= br then return ar < br end
        if a.questID and b.questID then
            return (tonumber(a.questID) or 0) < (tonumber(b.questID) or 0)
        end
        local an, bn = GetNodeDisplayName(a), GetNodeDisplayName(b)
        if an ~= bn then return an < bn end
        local aid = (a.itemID or a.achievementID or a.questID or a.mapID or a.instanceID
                  or a.visualID or a.gearSetID or a.titleID or 0)
        local bid = (b.itemID or b.achievementID or b.questID or b.mapID or b.instanceID
                  or b.visualID or b.gearSetID or b.titleID or 0)
        return tostring(aid) < tostring(bid)
    end)
end

local function GatherUncollectedNodes(node, out, activeKeys, seen)
    if type(node) ~= "table" then return end

    seen = seen or setmetatable({}, { __mode = "k" })
    if seen[node] then return end
    seen[node] = true

    local isAllowed, matched = IsAllowedLeaf(node, activeKeys)
    if isAllowed then
        out[#out + 1] = node
        passKeysByNode[node] = matched
--        DebugRecursive(node, "added uncollected node", 0, 1, false)
    end

    local kids = node.g
    if type(kids) == "table" then
        for i = 1, #kids do
            if type(kids[i]) == "table" and kids[i] ~= node.parent then
                GatherUncollectedNodes(kids[i], out, activeKeys, seen)
            end
        end
    end
end

local function BuildNodeList(root)
    local activeKeys = CollectActiveKeys()
    if #activeKeys == 0 then
        return {}, activeKeys
    end

    -- Gather raw leaves per active filters
    local nodes = {}
    GatherUncollectedNodes(root, nodes, activeKeys)

    -- Transformations (in order)
    nodes = DedupQuests(nodes)
    nodes = CollapseAchievementFamilies(root, nodes)
    nodes = GroupItemsByVisualID(nodes)
    SortPopupNodes(nodes)

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
    local btnName = "ATTGoGoListItem"..__rowSerial

    -- Create the button+label pair once
    local btn = CreateFrame("Button", btnName, scrollContent, "ItemButtonTemplate")
    btn:SetSize(32, 32)
    btn:RegisterForClicks("AnyUp")

    local label = scrollContent:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    label:SetWidth(220)
    label:SetJustifyH("LEFT")

    -- Click + tooltip
    btn:SetScript("OnClick", function(self, mouseButton)
        local node = self.node

        -- Ctrl+click: open Dressing Room; undress first if option is ON
        if IsModifiedClick("DRESSUP") and node and node.itemID then
            local link = select(2, GetItemInfo(node.itemID)) or ("item:" .. tostring(node.itemID))

            -- Bring up Blizzard's dressing room
            if DressUpFrame then
                if ShowUIPanel then pcall(ShowUIPanel, DressUpFrame) else DressUpFrame:Show() end
            end

            -- Model used by the dressing room across Classic/MoP UIs
            local mdl = _G.DressUpModel or (DressUpFrame and (DressUpFrame.Model or DressUpFrame.DressUpModel))
            if mdl and mdl.TryOn then
                if mdl.SetUnit then pcall(mdl.SetUnit, mdl, "player") end
                if GetSetting("dressUpNaked", true) and mdl.Undress then pcall(mdl.Undress, mdl) end
                pcall(mdl.TryOn, mdl, link)
            else
                -- Fallback (may keep current gear)
                if DressUpItemLink and link then DressUpItemLink(link) end
            end
            return
        end

        if IsModifiedClick("CHATLINK") or mouseButton == "MiddleButton" then
            Util.InsertNodeChatLink(node)
            return
        end

        if mouseButton == "LeftButton" and node then
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
            -- 3) Map-ish rows can focus the map
            if node.mapID or node.explorationID or node.instanceID or node.flightpathID then
                if Util.FocusMapForNode(node) then return end
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
    local y = -((dataIndex - 1) * ROW_HEIGHT) + 2
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
    if not uncollectedPopup then return end
    local nodes = uncollectedPopup.currentNodes or {}
    local scroller = uncollectedPopup.scrollFrame
    local content  = uncollectedPopup.scrollContent
    if not scroller or not content then return end

    -- viewport
    local viewH   = uncollectedPopup:GetHeight() - 45  -- header+padding
    if viewH < ROW_HEIGHT then viewH = ROW_HEIGHT end
    local first   = math.floor((scroller:GetVerticalScroll() or 0) / ROW_HEIGHT) + 1
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
local function EnsurePopup()
    if uncollectedPopup then return end

    uncollectedPopup = CreateFrame("Frame", "ATTGoGoUncollectedPopup", UIParent, BackdropTemplateMixin and "BackdropTemplate" or nil)
    uncollectedPopup:SetSize(268, 592)
    uncollectedPopup:SetClampedToScreen(true)
    uncollectedPopup:SetResizeBounds(180, 120, 800, 800)
    uncollectedPopup:SetResizable(true)
    uncollectedPopup:SetMovable(true)
    uncollectedPopup:EnableMouse(true)

    -- allow dragging by grabbing the frame OR the scroll area
    uncollectedPopup:RegisterForDrag("LeftButton")
    uncollectedPopup:SetScript("OnDragStart", function(self) self:StartMoving() end)
    uncollectedPopup:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        Util.SaveFramePosition(self, "popupWindowPos")
    end)

    -- look & strata
    uncollectedPopup:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 }
    })
    uncollectedPopup:SetFrameStrata("DIALOG")
    uncollectedPopup:SetFrameLevel(200)
    table.insert(UISpecialFrames, "ATTGoGoUncollectedPopup")

    -- title + close
    uncollectedPopup.title = uncollectedPopup:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    uncollectedPopup.title:SetPoint("TOP", 0, -10)
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

    -- allow dragging when grabbing inside the scroll area
    scroll:RegisterForDrag("LeftButton")
    scroll:SetScript("OnDragStart", function() uncollectedPopup:StartMoving() end)
    scroll:SetScript("OnDragStop",  function()
        uncollectedPopup:StopMovingOrSizing()
        Util.SaveFramePosition(uncollectedPopup, "popupWindowPos")
    end)

    -- scrollbar sync
    scroll:SetScript("OnScrollRangeChanged", function(self, _, yRange)
        local max = math.max(yRange or 0, 0)
        if self.ScrollBar then
            self.ScrollBar:SetMinMaxValues(0, max)
            if self.ScrollBar:GetValue() > max then
                self.ScrollBar:SetValue(max)
            end
        end
    end)
    scroll:SetScript("OnVerticalScroll", function(self, offset)
        local min, max = 0, 0
        if self.ScrollBar then
            local _, m = self.ScrollBar:GetMinMaxValues()
            max = m or 0
        end
        offset = math.max(min, math.min(offset or 0, max))
        self:SetVerticalScroll(offset)
        if self.ScrollBar then self.ScrollBar:SetValue(offset) end
        UpdateVirtualList()
    end)
    scroll:EnableMouseWheel(true)
    scroll:SetScript("OnMouseWheel", function(self, delta)
        local step = ROW_HEIGHT * 3
        local current = self:GetVerticalScroll()
        local min, max = 0, 0
        if self.ScrollBar then
            local _, m = self.ScrollBar:GetMinMaxValues()
            max = m or 0
        end
        local newOffset = math.max(min, math.min(current - delta * step, max))
        self:SetVerticalScroll(newOffset)
        if self.ScrollBar then self.ScrollBar:SetValue(newOffset) end
        UpdateVirtualList()
    end)
    if scroll.ScrollBar and not scroll.ScrollBar.__ATT_wired then
        scroll.ScrollBar.__ATT_wired = true
        scroll.ScrollBar:SetScript("OnValueChanged", function(sb, value)
            scroll:SetVerticalScroll(value or 0)
            UpdateVirtualList()
        end)
    end

    -- bottom-right resize grabber
    local resizer = CreateFrame("Button", nil, uncollectedPopup)
    resizer:SetSize(16, 16)
    resizer:SetPoint("BOTTOMRIGHT", -6, 6)
    resizer:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")
    resizer:SetHighlightTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Highlight")
    resizer:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Down")
    resizer:SetScript("OnMouseDown", function() uncollectedPopup:StartSizing("BOTTOMRIGHT") end)
    resizer:SetScript("OnMouseUp", function()
        uncollectedPopup:StopMovingOrSizing()
        Util.SaveFramePosition(uncollectedPopup, "popupWindowPos")
        UpdateVirtualList()
    end)

    -- persist pos/size + rerender on resize
    uncollectedPopup:SetScript("OnSizeChanged", function(self, w, h)
        if w and h then Util.SaveFramePosition(self, "popupWindowPos") end
        local scroller = self.scrollContent and self.scrollContent:GetParent()
        if scroller and scroller.UpdateScrollChildRect then scroller:UpdateScrollChildRect() end
        UpdateVirtualList()
    end)

    uncollectedPopup:SetScript("OnHide", function(self)
        HidePreview()
        Util.SaveFramePosition(self, "popupWindowPos")
    end)

    -- finally, restore last position/size
    Util.LoadFramePosition(uncollectedPopup, "popupWindowPos", "RIGHT", -200, 64)
end

------------------------------------------------------------
-- Populate & refresh (virtualized)
------------------------------------------------------------
local function PopulateUncollectedPopup(scrollContent, nodes)
    -- Hide any leftover visuals (rows are reused)
    for _, child in ipairs({ scrollContent:GetChildren() }) do
        if child.Hide then child:Hide() end
    end
    for _, r in ipairs({ scrollContent:GetRegions() }) do
        if r and r.Hide then r:Hide() end
    end

    if #nodes == 0 then
        scrollContent.emptyLine = scrollContent.emptyLine
            or scrollContent:CreateFontString(nil, "OVERLAY", "GameFontNormal")
        local line = scrollContent.emptyLine
        line:ClearAllPoints()
        line:SetPoint("TOPLEFT", 5, 0)
        line:SetText("All collected!")
        line:Show()
        scrollContent:SetHeight(40)
    else
        if scrollContent.emptyLine then scrollContent.emptyLine:Hide() end
        scrollContent:SetHeight(#nodes * ROW_HEIGHT + 10)
    end

    local scroll = scrollContent:GetParent()
    if scroll and scroll.UpdateScrollChildRect then scroll:UpdateScrollChildRect() end
    if scroll then
        scroll:SetVerticalScroll(0)
        if scroll.ScrollBar then scroll.ScrollBar:SetValue(0) end
    end

    UpdateVirtualList()
end

local function PopupLazyRefresh(self)
    if not (self and self:IsShown() and self.currentData) then return end

    local ok, nodes, _ = pcall(function() return BuildNodeList(self.currentData) end)
    if not ok then return end
    self.currentNodes = nodes or {}

    pcall(PopulateUncollectedPopup, self.scrollContent, self.currentNodes)
end

------------------------------------------------------------
-- Data-updater frame (late item/spell names)
------------------------------------------------------------
local updater = CreateFrame("Frame")
local function TryRegister(ev) pcall(updater.RegisterEvent, updater, ev) end
TryRegister("GET_ITEM_INFO_RECEIVED")
TryRegister("SPELLS_CHANGED")

updater:SetScript("OnEvent", function(_, event, a1)
    if event == "GET_ITEM_INFO_RECEIVED" then
        local itemID = a1
        local entry = itemLabelsByID[itemID]
        if entry then
            local name = GetItemInfo(itemID)
            if name then
                entry.label:SetText(name)
                if entry.btn then Util.ApplyNodeIcon(entry.btn, entry.btn.node) end
                itemLabelsByID[itemID] = nil
            end
        end
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

    local ok, nodes, activeKeys = pcall(function() return BuildNodeList(data) end)
    if not ok then
        nodes, activeKeys = {}, {}
    end
    uncollectedPopup.currentNodes = nodes

    pcall(PopulateUncollectedPopup, uncollectedPopup.scrollContent, nodes)

    uncollectedPopup.title:SetText(Util.NodeDisplayName(data))

    -- lazy refresh to update late-resolving names/icons
    C_Timer.After(1.0, function()
        if uncollectedPopup and uncollectedPopup:IsShown() then
            local ok3, err3 = pcall(PopupLazyRefresh, uncollectedPopup)
        end
    end)
end

function ShowUncollectedPopup(data)
--    DebugPrintNodePath(data, { verbose = true })
--    if data.parent then DebugRecursive(data.parent, "popup.parent", 0, 1, false) end
--    DebugRecursive(data, "popup.data", 0, 2, false)

    EnsurePopup()
    RefreshPopup(data)
    uncollectedPopup:Show()
end
