-- === Utility Functions ===

-- all the global variables/tables/etc are in this file to ensure smooth access with no order dependencies

ATT = _G.AllTheThings

-- SavedVariables tables
ATTGoGoDB     = ATTGoGoDB     or {}
ATTGoGoCharDB = ATTGoGoCharDB or {}

COLLECTIBLE_ID_FIELDS = {
    achievementID = true,
    creatureID = true,
    explorationID = true,
    flightpathID = true,
    itemID = true,
    mapID = false,
    questID = false,
    titleID = true,
    visualID = true,
}

COLLECTIBLE_ID_LABELS = {
    achievementID = "achievement",
    creatureID = "creature",
    explorationID = "exploration",
    flightpathID = "flight path",
    itemID = "item",
    mapID = "map",
    questID = "quest",
    titleID = "title",
    visualID = "visual",
}

function bool(v) return v and true or false end

Util = {}

-- === Account-scoped settings ===
function GetSetting(key, default)
    local v = ATTGoGoDB[key]
    if v == nil then return default end
    return v
end

function SetSetting(key, value)
    ATTGoGoDB[key] = value
end

-- === Character-scoped settings ===
function GetCharSetting(key, default)
  local v = ATTGoGoCharDB[key]
  if v == nil then return default end
  return v
end

function SetCharSetting(key, value)
  ATTGoGoCharDB[key] = value
end

-------------------------------------------------
-- Small helpers
-------------------------------------------------
function Util.PlayerFactionID()
  local f = UnitFactionGroup("player")
  if f == "Alliance" then return 2
  elseif f == "Horde" then return 1
  else return 0   -- Neutral / unknown
  end
end

function Util.Debounce(fn, delay)
  local pending = false
  delay = delay or 0.05
  return function(...)
    local args = { ... }
    if pending then return end
    pending = true
    C_Timer.After(delay, function() pending = false; fn(unpack(args)) end)
  end
end

function Util.FormatTime(seconds)
  local days    = math.floor(seconds / 86400)
  local hours   = math.floor((seconds % 86400) / 3600)
  local minutes = math.floor((seconds % 3600) / 60)
  return (days > 0 and (days .. "d ") or "") .. ((days > 0 or hours > 0) and (hours .. "h ") or "") .. minutes .. "m"
end

function Util.InsertNodeChatLink(node)
  if not node then TP(node); return end
  local link
  if     node.itemID        then link = select(2, GetItemInfo(node.itemID))
  elseif node.achievementID then link = GetAchievementLink(node.achievementID)
  elseif node.spellID       then link = GetSpellLink(node.spellID)
  elseif node.questID       then link = GetQuestLink(node.questID)
  end
  if link and not ChatEdit_InsertLink(link) then ChatFrame_OpenChat(link) end
end

function Util.NodeDisplayName(n)
  return n.text or n.name
      or (n.mapID and ("Map " .. n.mapID))
      or (n.instanceID and ("Instance " .. n.instanceID))
      or "?"
end

local _ATT_ONE_CACHE = setmetatable({}, { __mode = "v" })  -- weak values

function Util.ClearATTSearchCache() wipe(_ATT_ONE_CACHE) end

function Util.ATTSearchOne(field, id)
  local k = field .. ":" .. id
  local hit = _ATT_ONE_CACHE[k]
  if hit ~= nil then return hit end
--  local done = ATTPerf.auto("ATT.SearchForObject/Field")
--  hit = ATT.SearchForObject(field, id, "field")  -- strict search
--     or ATT.SearchForObject(field, id)           -- less strict alternative
--     or (ATT.SearchForField(field, id))[1]       -- least strict fallback
--  done()
  hit = ATTPerf.wrap("SearchForObject 1", function() return ATT.SearchForObject(field, id, "field") end)
     or ATTPerf.wrap("SearchForObject 2", function() return ATT.SearchForObject(field, id) end)
     or ATTPerf.wrap("SearchForObject 3", function() return (ATT.SearchForField(field, id))[1] end)
  if hit ~= nil then _ATT_ONE_CACHE[k] = hit end
  return hit
end

local _MAP_ROOT_CACHE     = {}
local _MAP_PROG_CACHE = {}

function Util.InvalidateMapProgress(mapID)
  if mapID then _MAP_PROG_CACHE[mapID] = nil
  else wipe(_MAP_PROG_CACHE) end
end

-- Wrap a map package in a simple root so our popup can recurse it like any ATT node
function Util.GetMapRoot(mapID)
  local hit = _MAP_ROOT_CACHE[mapID]
  if hit ~= nil then return hit or nil end        -- false sentinel means "known empty"
  local pkg = ATTPerf.wrap("GetCachedDataForMapID", function() return ATT.GetCachedDataForMapID(mapID) end)
  if type(pkg) ~= "table" or not next(pkg) then
    TP(pkg, next(pkg))
    _MAP_ROOT_CACHE[mapID] = false                -- remember "no data" to avoid repeated work
    return nil
  end
  local info = C_Map.GetMapInfo(mapID)
  local name = (info and info.name) or ("Map " .. mapID)
  local kids = (type(pkg.g) == "table" and pkg.g) or pkg
  local root = { text = name, name = name, mapID = mapID, g = kids }
  _MAP_ROOT_CACHE[mapID] = root
  return root
end

-- Progress straight from the map package (matches /attmini totals)
function Util.ResolveMapProgress(mapID)
  local hit = _MAP_PROG_CACHE[mapID]
  if hit then return hit[1], hit[2], hit[3] end
  local root = ATTPerf.wrap("GetMapRoot", Util.GetMapRoot, mapID)
  if type(root) == "table" then
    local c, t, p = ATTPerf.wrap("ATTGetProgress", Util.ATTGetProgress, root)
    _MAP_PROG_CACHE[mapID] = {c, t, p}
    return c, t, p
  else
    TP(mapID, root)
  end
  return 0, 0, 0
end

-- === Favorites (account-wide) ===
function Util.Favorites()
  local t = GetSetting("favorites", nil)
  if type(t) ~= "table" then t = {}; SetSetting("favorites", t) end
  return t
end

function Util.IsFavoriteKey(key)
  return Util.Favorites()[key] == true
end

function Util.ToggleFavoriteKey(key)
  local t = Util.Favorites()
  t[key] = not t[key] or nil
  SetSetting("favorites", t)
end

-- Stable keys per widget
function Util.FavKey(obj, isZone)
  if isZone then
    -- obj can be a tile/entry with mapID or a raw mapID
    return "Z" .. (obj.mapID or obj)
  else
    -- obj can be a tile data, entry with attNode, or an ATT node
    return "I" .. Util.GetInstanceProgressKey(obj.attNode or obj)
  end
end

-------------------------------------------------
-- Progress resolution
-------------------------------------------------
local _PROG_CACHE = setmetatable({}, { __mode = "k" })
function Util.InvalidateProgressCache(node)
  -- no arg => nuke all (use for BIG wave / full rebuild)
  if not node then
    for k in pairs(_PROG_CACHE) do _PROG_CACHE[k] = nil end
    return
  end
  -- targeted: clear this node and bubble to parents so rollups recompute
  local p = node
  while type(p) == "table" do
    _PROG_CACHE[p] = nil
    p = rawget(p, "parent")
  end
end

function Util.ATTGetProgress(node)
  if not node then TP(node, node.g); return 0, 0, 0 end

  local hit = _PROG_CACHE[node]
  if hit then return hit[1], hit[2], hit[3] end

  if type(node.g) ~= "table" then return 0, 0, 0 end
  if node.collectible then
    return node.collected and 1 or 0, 1, node.collected and 100 or 0
  end

  local c = node.progress or 0
  local t = node.total or 0
  if t > 0 then return c, t, (c / t) * 100 end
  if next(node.g) ~= nil then
    local ac, at = 0, 0
    for _, child in pairs(node.g) do
      local c1, t1 = Util.ATTGetProgress(child)
      ac, at = ac + c1, at + t1
    end
    if at > 0 then
      local ap = (ac / at) * 100
      _PROG_CACHE[node] = { ac, at, ap }
      return ac, at, ap
    end
  else
    TP(node, node.g, next(node.g))
  end
  return 0, 0, 0
end

function Util.GetCollectionProgress(dataset)
  local c, t = 0, 0
  if type(dataset) ~= "table" then TP(dataset); return 0, 0, 0 end

  local added = false
  for _, entry in pairs(dataset) do
    local node = (type(entry) == "table" and (entry.attNode or entry)) or nil
    if node then
      local c1, t1 = Util.ATTGetProgress(node)
      c, t = c + (c1 or 0), t + (t1 or 0)
      added = true
    else
      TP(added, entry)
    end
  end

  if not added then
    TP(dataset)
  end
  return c, t, (t > 0) and (c / t * 100) or 0
end

-------------------------------------------------
-- Frame helpers
-------------------------------------------------
-- Save frame position (+size) to DB
function Util.SaveFramePosition(frame, dbKey)
    local point, _, relativePoint, xOfs, yOfs = frame:GetPoint()
    local w, h = frame:GetSize()

    local DB = ATTGoGoCharDB
    DB[dbKey] = DB[dbKey] or {}
    local rec = DB[dbKey]
    rec.point = point
    rec.relativePoint = relativePoint
    rec.xOfs = xOfs
    rec.yOfs = yOfs
    rec.width  = math.floor(w + 0.5)
    rec.height = math.floor(h + 0.5)
end

-- Load frame position (+size) from DB with defaults
function Util.LoadFramePosition(frame, dbKey, defaultPoint, defaultX, defaultY)
    local pos = ATTGoGoCharDB[dbKey]
    frame:ClearAllPoints()
    if pos then
        frame:SetPoint(pos.point or "CENTER", UIParent, pos.relativePoint or "CENTER", pos.xOfs or 0, pos.yOfs or 0)
        if frame:IsResizable() and pos.width and pos.height then
            frame:SetSize(pos.width, pos.height)
        end
    else
        frame:SetPoint(defaultPoint or "CENTER", defaultX or 0, defaultY or 0)
    end
end

-- Make a frame draggable and persist its position
function Util.EnableDragPersist(frame, dbKey)
    frame:SetMovable(true); frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        Util.SaveFramePosition(self, dbKey)
    end)
end

-- Let a ScrollFrame drag its owner window (and persist)
function Util.EnableScrollDrag(scrollFrame, ownerFrame, dbKey)
    scrollFrame:RegisterForDrag("LeftButton")
    scrollFrame:SetScript("OnDragStart", function() ownerFrame:StartMoving() end)
    scrollFrame:SetScript("OnDragStop",  function()
        ownerFrame:StopMovingOrSizing()
        Util.SaveFramePosition(ownerFrame, dbKey)
    end)
end

-- Persist on any size change
function Util.PersistOnSizeChanged(frame, dbKey, onSizeChanged)
    frame:HookScript("OnSizeChanged", function(self, w, h)
        Util.SaveFramePosition(self, dbKey)
        onSizeChanged(self, w, h)
    end)
end

-- Add the standard bottom-right resizer to a frame
function Util.AddResizerCorner(frame, dbKey, onDone)
    local grab = CreateFrame("Button", nil, frame)
    grab:SetSize(16, 16)
    grab:SetPoint("BOTTOMRIGHT", -6, 6)
    grab:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")
    grab:SetHighlightTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Highlight")
    grab:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Down")
    grab:SetScript("OnMouseDown", function() frame:StartSizing("BOTTOMRIGHT") end)
    grab:SetScript("OnMouseUp", function()
        frame:StopMovingOrSizing()
        Util.SaveFramePosition(frame, dbKey)
        onDone()
    end)
end


function Util.ClearChildrenOrTabs(arg)
  local done = ATTPerf.auto("Util.ClearChildrenOrTabs")
  -- anything derived from Frame has a .GetChildren()
  if arg and type(arg) ~= "string" and arg.GetChildren then
    for _, child in ipairs({ arg:GetChildren() }) do
      child:Hide()
      child:SetParent(nil)
    end
    return
  end
  if type(arg) == "table" then
    for k, v in pairs(arg) do
      if type(v) == "table" and v.Hide and v.SetParent then v:Hide(); v:SetParent(nil) else TP(k, v, arg[k]) end
      arg[k] = nil
    end
    wipe(arg)
  else
    TP(arg)
  end
  done()
end

function Util.GetGridCols(scrollWidth, widgetSize, padding)
  local cols = math.floor((scrollWidth + padding) / (widgetSize + padding))
  return (cols < 1) and 1 or cols
end

function Util.SetTooltip(frame, anchor, title, ...)
  local n    = select("#", ...)
  local args = { ... }

  Tooltip.CreateTooltip(frame, anchor, function()
    Tooltip.AddHeader(title)
    for i = 1, n do Tooltip.AddLine(args[i]) end
  end)
end

-- Per-char popup id-filters (merge defaults)
function Util.CanonicalizePopupIdFilters()
  ATTGoGoCharDB.popupIdFilters = ATTGoGoCharDB.popupIdFilters or {}
  local t = ATTGoGoCharDB.popupIdFilters
  for k in pairs(t) do
    if COLLECTIBLE_ID_FIELDS[k] == nil then t[k] = nil end
  end
  for k, def in pairs(COLLECTIBLE_ID_FIELDS) do
    if t[k] == nil then t[k] = def end
  end
end

function Util.SetPopupIdFilter(key, value)
  ATTGoGoCharDB.popupIdFilters[key] = value
end

-------------------------------------------------
-- Achievement helpers
-------------------------------------------------
function Util.OpenAchievementByID(achievementID)
  if not achievementID then TP(achievementID); return end

  if IsModifiedClick("CHATLINK") then
    local link = GetAchievementLink(achievementID)
    if link then ChatEdit_InsertLink(link) return else TP(link) end
  end

  UIParentLoadAddOn("Blizzard_AchievementUI")
  OpenAchievementFrameToAchievement(achievementID)
  ShowUIPanel(AchievementFrame)
  AchievementFrame_SelectAchievement(achievementID)
end

-- Given a node (often a Title leaf), try to find the achievement that awards it.
function Util.FindAchievementForTitleNode(node)
  if type(node) ~= "table" then TP(node); return nil end
  -- If the node already carries an achievementID, use it.
  if node.achievementID then return node.achievementID end

  -- Walk up parents to see if it’s embedded under an achievement.
  local function ascend_for_achievement(n)
    local cur, hops = n, 0
    while type(cur) == "table" and hops < 6 do
      if cur.achievementID then return cur.achievementID end
      cur = rawget(cur, "parent"); hops = hops + 1
    end
  end
  local up = ascend_for_achievement(node)
  if up then return up end

  -- If we have a titleID, ask ATT for that leaf and walk *its* parents.
  if node.titleID and Util.ATTSearchOne then
    local hit = Util.ATTSearchOne("titleID", node.titleID)
    if type(hit) == "table" then
      local via = ascend_for_achievement(hit)
      if via then return via end
      if hit.achievementID then return hit.achievementID end
    end
  end

  -- No luck.
  TP(node, node.name, node.parent, node.parent.name, node.achievementID, node.titleID, up)
  return nil
end

-------------------------------------------------
-- Map/waypoint helpers
-------------------------------------------------
function Util.ExtractMapAndCoords(node)
  if not node or type(node) ~= "table" then TP(node); return nil end

  local c = node.coords
  if c then
    local x = c[1][1]
    local y = c[1][2]
    local m = c[1][3]
    if x and y then
      if x > 1 then x = x / 100 end
      if y > 1 then y = y / 100 end
      if x < 0 then x = 0 elseif x > 1 then x = 1 end
      if y < 0 then y = 0 elseif y > 1 then y = 1 end
      local mapID = m or node.mapID
      if mapID then return mapID, x, y else TP(m, x, y, mapID) end
    end
  end

  return nil
end

function Util.TryTomTomWaypoint(mapID, x, y, title)
  if not (mapID and C_Map.GetMapInfo(mapID)) then TP(mapID, title); return false end
  if TomTom and TomTom.AddWaypoint then
    TomTom:AddWaypoint(mapID, x, y, { title = title, persistent = false })
    return true
  end
  return false
end

function Util.FocusMapForNode(node)
  local mapID, x, y = Util.ExtractMapAndCoords(node)
  if not mapID and node.instanceID then
    local inst = Util.ATTSearchOne("instanceID", node.instanceID)
    if inst then mapID, x, y = Util.ExtractMapAndCoords(inst) end
  end
  if not mapID and node.flightpathID and node.g then
    for _, ch in ipairs(node.g) do mapID, x, y = Util.ExtractMapAndCoords(ch); if mapID then break end end
  end
  if not mapID and node.parent then
    mapID, x, y = Util.ExtractMapAndCoords(node.parent)
  end

  if not mapID then return false end

  ShowUIPanel(WorldMapFrame); WorldMapFrame:SetMapID(mapID) -- open WorldMap for the `mapID`'s zone

  if x and y then Util.TryTomTomWaypoint(mapID, x, y, node.text or node.name or (TITLE .. " waypoint")) end
  return true
end

function Util.GetNodeIcon(node)
  if not node then TP(node); return nil end
  if node.icon then return node.icon end

  -- meta-achievement icon via Blizzard API (covers our stub reps)
  if node.achievementID then
    local _, _, _, _, _, _, _, _, _, icon = GetAchievementInfo(node.achievementID)
    if icon then return icon end
  end

  -- spell icons
  if node.spellID then
    local icon = GetSpellTexture(node.spellID)
    if icon then return icon end
  end

  -- Fallback: scan ONLY this node's fields for "*ID" and ask ATT for an icon
  for field, id in pairs(node) do
    if type(field) == "string" and field:sub(-2) == "ID" and id ~= nil then
      local res = Util.ATTSearchOne(field, id)
      if res.icon and res.icon ~= 0 and res.icon ~= "" then return res.icon else TP(field, id, res) end
    end
  end

  -- bubble up a few parents if needed (ATT may populate later for items)
  local p, hops = rawget(node, "parent"), 0
  while p and hops < 3 do
    if p.icon then return p.icon end
    p = rawget(p, "parent"); hops = hops + 1
  end
  TP(node, node.parent, p, hops)

  return nil
end

-- Centralized icon applier: works with ItemButtons *and* raw Textures.
-- Usage:
--   Util.ApplyNodeIcon(btnOrTexture, node)
--   Util.ApplyNodeIcon(btnOrTexture, node, { texCoord = {0.07,0.93,0.07,0.93} })
function Util.ApplyNodeIcon(target, node, opts)
  opts = opts or {}
  local tex = Util.GetNodeIcon(node)   -- may be file path, fileID, atlas, or a table {atlas=..., coords=..., id=..., texture=...}
  local icon
  -- Determine the "icon" subtexture if target is an ItemButton; otherwise treat target as the Texture itself.
  if target and target.GetObjectType and target:GetObjectType() == "Texture" then
    icon = target
  else
    icon = (target and (target.icon or target.Icon or target.IconTexture))
        or (target and target.GetName and _G[target:GetName() .. "IconTexture"])
  end

  local function clear_icon()
    if icon then icon:SetAtlas(nil) end
    if icon then icon:SetTexture(nil) end
    if icon then icon:SetTexCoord(0, 1, 0, 1) end
    if icon then icon:SetDesaturated(false) end
  end
  local function normalize_path(s)
    s = s:gsub("\\", "/")
    s = s:gsub("^interface/", "Interface/")
    s = s:gsub("^Interface/addons/", "Interface/AddOns/")
    if not s:find("^Interface/") then s = "Interface/" .. s end
    return s
  end
  local function apply_file(file_or_id, coords)
    clear_icon()
    -- If we have a Texture, set it directly; otherwise try ItemButton texture path.
    if icon and not icon.GetObjectType then TP(icon) end
    if icon and not icon.SetTexture then TP(icon) end
    if icon and icon.GetObjectType and icon:GetObjectType() == "Texture" then
      icon:SetTexture(file_or_id or 134400)
    elseif SetItemButtonTexture and target then
      SetItemButtonTexture(target, file_or_id or 134400)
    elseif icon and icon.SetTexture then
      icon:SetTexture(file_or_id or 134400)
    end
    if target and not SetItemButtonTexture then TP(file_or_id, coords) end
    local tc = opts.texCoord or coords
    if icon and tc and #tc == 4 then
      icon:SetTexCoord(tc[1], tc[2], tc[3], tc[4])
    end
  end
  local function apply_atlas(atlas, desat)
    if icon and not icon.SetAtlas then TP(atlas, desat) end
    if icon and icon.SetAtlas then
      clear_icon()
      icon:SetAtlas(atlas, false)
      icon:SetDesaturated(desat)
      return true
    end
    TP(atlas, desat)
    return false
  end

  -- String: atlas name (no slash/dot) OR file path
  if type(tex) == "string" then
    if (not tex:find("/")) and (not tex:find("%.")) then
      TP(target, node, opts, tex)
      if not apply_atlas(tex, false) then
        apply_file(134400)
      end
      return
    else
      apply_file(normalize_path(tex))
      return
    end
  end

  -- Numeric fileID
  if type(tex) == "number" then
    apply_file(tex)
    return
  end

  -- Fallback
  TP(target, node, opts)
  apply_file(134400)
end

-- === Removed/retired detection ===
local BUILD_NO = select(4, GetBuildInfo())

-- return true if a node should be considered 'removed from game' relative to current client
function Util.IsNodeRemoved(n)
return ATTPerf.wrap("Util.IsNodeRemoved", function()
  if n.u == 2 then return true end           -- ATT unobtainable flag for removed content
  if n.rwp then return n.rwp <= BUILD_NO end -- rwp: removed with patch
  if n.awp then return n.awp > BUILD_NO end  -- awp: added with patch
  return false
end)
end

-- === Era helpers ===
local function EraFromAwp(awp)
  if not awp then return nil end
  local era = math.floor(awp / 10000)
  if era <= 0 then return 1 end
  if era >= 11 then return nil end
  return era
end

-- Return era for a difficulty child (prefer child.awp, then instance.awp, then instance.expansionID, else Classic)
local function EraForChild(instanceNode, child)
  if not child then TP(instanceNode, child) end
  if child and child.difficultyID then
    return EraFromAwp(child.awp)
        or EraFromAwp(instanceNode.awp)
        or instanceNode.expansionID
        or 1
  end
  return nil
end

-- Build { [era] = {difficultyChildren...} } ignoring non-difficulty headers
local function BuildEraBuckets(instanceNode)
  local buckets, hasDiff = {}, false
  local kids = type(instanceNode.g) == "table" and instanceNode.g or nil
  if kids then
    for _, ch in pairs(kids) do
      if ch.difficultyID then
        hasDiff = true
        local era = EraForChild(instanceNode, ch)
        if era then
          local t = buckets[era] or {}
          t[#t+1] = ch
          buckets[era] = t
        else
          TP(instanceNode, instanceNode.g, kids, ch, era)
        end
      end
    end
  else
    TP(instanceNode, instanceNode.g, kids)
  end

  if not hasDiff then
    -- no difficulty children -> one bucket (non-split)
    local era = EraFromAwp(instanceNode.awp) or instanceNode.expansionID or 1
    buckets[era] = {}
  end
  return buckets
end

-- Wrapper limited to era; also decide and *store once* a stable progress key
local function MakeEraWrapper(instanceNode, era, diffs, isSplit)
  local name = instanceNode.text or instanceNode.name
  local wrap = {
    text = name, name = name,
    instanceID = instanceNode.instanceID,
    mapID = instanceNode.mapID,
    savedInstanceID = instanceNode.savedInstanceID,  -- keep lock-match key on the wrapper
    icon = instanceNode.icon,
    parent = instanceNode.parent,
    g = (type(diffs)=="table" and #diffs>0) and diffs or instanceNode.g,
    awp = instanceNode.awp,
    rwp = instanceNode.rwp,
    eraKey = era,
  }
  -- progressKey: for non-split keep numeric instanceID (back-compat); for split include era
  local id = instanceNode.instanceID
  if isSplit then
    wrap.__eraSplit = true
    wrap.progressKey = id .. ":" .. era
  else
    wrap.progressKey = id
  end
  return wrap
end

-------------------------------------------------
-- ATT instance resolvers & zone helper
-------------------------------------------------

-- from an Instance node, pick the child Group which matches a difficultyID
function Util.SelectDifficultyChild(instanceNode, difficultyID)
  if not (instanceNode and instanceNode.g) then TP(instanceNode, difficultyID); return nil end

  for _, child in ipairs(instanceNode.g) do
    if child.difficultyID == difficultyID then
        return child
    end
  end

  return instanceNode
end

-- Unified context resolver: returns the ATT node for current instance or zone.
-- Returns: node, info  where info={kind="instance"|"zone", uiMapID=?}
function Util.ResolveContextNode()
return ATTPerf.wrap("Util.ResolveContextNode", function()
  local info = {}
  local sentinel = { text = "Unknown instance", name = "Unknown instance", g = {} }

  if IsInInstance() then
    local _, instType,_,_,_,_,_, instID = GetInstanceInfo()
    if instType == "party" or instType == "raid" then
      info.kind = "instance"
      -- 1) Find the raw instance node by instanceID (fallback to mapID)
      -- some ATT nodes, like Kara, are not found by `instID == 532` but is found by `mapID == 350` (or Temple of Jade Serpent 464/429)
      local node = Util.ATTSearchOne("instanceID", instID)
      if not node then
          local mapID = C_Map.GetBestMapForUnit("player")
          node = Util.ATTSearchOne("mapID", mapID) or TP("no node by instance or map ID", GetInstanceInfo(), mapID)
      end
      if not node then return sentinel, info end

      -- 2) Narrow to the current difficulty
      local curDiff = ATT.GetCurrentDifficultyID()
      local child   = Util.SelectDifficultyChild(node, curDiff) or node  -- returns diff child, or the node itself. 

      -- 3) Determine current era + whether the instance is era-split, then wrap
      local buckets = BuildEraBuckets(node)                               -- { [era] = {diff-children...} } 
      local first   = next(buckets)
      local isSplit = bool(first and next(buckets, first))
      -- Prefer child’s era; fall back to the first bucket / expansionID / Classic
      local era     = (child.difficultyID and (EraFromAwp(child.awp) or EraFromAwp(node.awp) or node.expansionID or 1))
                      or first
                      or node.expansionID
                      or 1
      -- we want the *current difficulty branch* in the returned node
      local diffs   = { child }      -- hand the wrapper a single diff-branch

      local cooked  = MakeEraWrapper(node, era, diffs, isSplit)
      return cooked, info
    end
  end

  -- treat everything else as a "zone"
  info.kind = "zone"
  info.uiMapID = C_Map.GetBestMapForUnit("player")
  return Util.ATTSearchOne("mapID", info.uiMapID) or TP(info) or sentinel, info
end)
end

function Util.ResolvePopupTargetForCurrentContext()
  local node, info = Util.ResolveContextNode()
  if info.kind == "instance" then
    return Util.SelectDifficultyChild(node, ATT.GetCurrentDifficultyID()) or node
  else
    return Util.GetMapRoot(info.uiMapID)
  end
end

function IsInstanceLockedOut(instance)
  local sid
  if type(instance) == "table" then
    local n = instance
    while type(n) == "table" and not sid do sid = tonumber(n.savedInstanceID); n = n.parent end
  else
    sid = tonumber(instance)
  end
  if not sid then TP(instance, sid); return false end
  for i = 1, GetNumSavedInstances() do
    local _, _, _, _, locked, _, _, _, _, _, numEncounters, numCompleted, _, savedInstanceID = GetSavedInstanceInfo(i)
    if locked and tonumber(savedInstanceID) == sid then
      return true, (numCompleted or 0), (numEncounters or 0), i
    end
  end
  return false
end

-- ============================================================
-- Persist per-character progress for instances & zones
-- Layout (arrays, in c/t order):
--   ATTGoGoDB.progress[<realm>][<char>].instances[instanceID] = {
--       [1]=c, [2]=t,
--       lock = { expiresAt=<epoch>, bosses = { { name=<string>, down=<bool> }, ... } } | nil
--   }
--   ATTGoGoDB.progress[<realm>][<char>].zones[mapID] = { [1]=c, [2]=t }
-- ============================================================

-- Cached DB bucket (immutable after first build for this session)
local _AGG_ProgressCache -- { me=<table>, realm=<string>, char=<string> }

function Util.EnsureProgressDB()
  if _AGG_ProgressCache then
    return _AGG_ProgressCache.me, _AGG_ProgressCache.realm, _AGG_ProgressCache.char
  end

  local prog = ATTGoGoDB.progress
  if type(prog) ~= "table" then prog = {}; ATTGoGoDB.progress = prog end

  --local realm = GetRealmName() or "?"
  local realm = GetRealmName()
  if not realm then TP(realm); realm = "?" end
  prog[realm] = prog[realm] or {}

  --local charName = UnitName("player") or "?"
  local charName = UnitName("player")
  if not charName then TP(charName); charName = "?" end
  local byChar = prog[realm]
  -- always reset this toon’s layout (new schema every load)
  byChar[charName] = {
--    locks = {},        -- [instanceID] = lock snapshot
    instances  = {},   -- ["<instanceID>:<era>"] = { c, t }
    zones = {},        -- [mapID] = { c, t }
  }

  _AGG_ProgressCache = { me = byChar[charName], realm = realm, char = charName }
  return _AGG_ProgressCache.me, realm, charName
end

-- Build a lockout snapshot (absolute expiry + boss list by name only)
local function BuildLockoutFromSavedInstances(attInstanceNode)
  local isLocked, _, numBosses, lockoutIndex = IsInstanceLockedOut(attInstanceNode)
  if not isLocked then return nil end

  local _, _, reset, _, _, _, _, _, _, _, _, _, _, sid = GetSavedInstanceInfo(lockoutIndex)
  local expiresAt = time() + reset

  local bosses = {}
  for i = 1, numBosses do
    local bossName, _, killed = GetSavedInstanceEncounterInfo(lockoutIndex, i)
    bosses[#bosses+1] = { name = bossName or ("Boss " .. i), down = killed }
  end

  return { expiresAt = expiresAt, bosses = bosses, sid = sid }
end

function Util.SaveInstanceProgressByNode(attInstanceNode)
  if type(attInstanceNode) ~= "table" then TP(attInstanceNode); return end
  local instanceID = attInstanceNode.instanceID; if not instanceID then TP(attInstanceNode); return end

  local key = Util.GetInstanceProgressKey(attInstanceNode) or TP(attInstanceNode) or instanceID
  local c, t = Util.ATTGetProgress(attInstanceNode)
  if not t then
    TP(attInstanceNode, instanceID, key, c, t)
  end
  local lock = BuildLockoutFromSavedInstances(attInstanceNode)

  local me = Util.EnsureProgressDB()
  me.instances = me.instances or {}

  if key == instanceID then
    -- non-split: progress + lock under numeric key
    me.instances[instanceID] = { c, t, lock = lock }
  else
    -- split: store progress under composite key; keep lock under numeric ID
    DebugLogf("saving era-split instance progress with instanceID %d, key %s, c %d, t %d", instanceID, key, c, t)
    me.instances[key] = { c, t }
    local base = me.instances[instanceID] or {}
    base = { c, t, lock = lock }
    me.instances[instanceID] = base
  end
end

-- Save zone progress for an ATT zone node
function Util.SaveZoneProgressByMapID(mapID)
  if not mapID then TP(mapID); return end
  local c, t = Util.ResolveMapProgress(mapID)
  Util.EnsureProgressDB().zones[mapID] = { c or 0, t or 0 }
end

-- Convenience: snapshot whatever the current context is
function Util.SaveCurrentContextProgress()
  local node, info = Util.ResolveContextNode()

  if info.kind == "instance" then
    Util.SaveInstanceProgressByNode(node)
  else
    if info.uiMapID then Util.SaveZoneProgressByMapID(info.uiMapID) else TP(node, info) end
  end
end

-- === Other-toons option (tri-state) ===
function Util.GetOtherToonsMode()
  -- 0 = off, 1 = instances with lockouts, 2 = zones+instances
  local v = tonumber(GetSetting("otherToonsInTooltips", 1)) or 1
  if v < 0 or v > 2 then v = 1 end
  return v
end

-------------------------------------------------
-- UI lists (Expansions / Zones)
-------------------------------------------------
function GetCompletionColor(percent)
  percent = math.max(0, math.min(100, percent or 0))
  local intensity = 0.15
  if percent < 50 then
    local g = percent / 50; return intensity, g * intensity, 0
  else
    local r = (100 - percent) / 50; return r * intensity, intensity, 0
  end
end

local function CompletionHex(percent, boost)
  local r, g, b = GetCompletionColor(percent)
  boost = boost or 4.0                      -- punch it up for text
  r, g, b = math.min(r*boost,1), math.min(g*boost,1), math.min(b*boost,1)
  return string.format("|cff%02x%02x%02x", r*255, g*255, b*255)
end

function BuildExpansionList()
return ATTPerf.wrap("BuildExpansionList", function()
  local list, seen = {}, {}
  local root = ATT:GetDataCache()

  local function scanContainer(cat)
    if type(cat.g) ~= "table" then return end
    for _, exp in pairs(cat.g) do
      local id = exp and exp.expansionID
      if id and type(exp.g) == "table" and next(exp.g) ~= nil and not seen[id] then
        local hasInstance = false
        for _, c in pairs(exp.g) do if c and c.instanceID then hasInstance = true break end end
        if hasInstance then
          seen[id] = true
          list[#list+1] = { id = id, name = exp.text or ("Expansion " .. id), node = exp }
        end
      end
    end
  end

  for _, cat in pairs(root.g) do scanContainer(cat) end
  table.sort(list, function(a,b) return (a.id or 0) < (b.id or 0) end)
  return list
end)
end

function Util.GetInstanceProgressKey(node)
  if type(node) ~= "table" then TP(node); return nil end
  if node.progressKey ~= nil then return node.progressKey end
  local id = node.instanceID; if not id then TP(id); return nil end
  local era = node.eraKey
  -- if we don’t know whether it’s split, default to legacy (numeric)
  node.progressKey = (node.__eraSplit and era) and (id .. ":" .. era) or id
  return node.progressKey
end

function GetInstancesForExpansion(expansionID)
return ATTPerf.wrap("GetInstancesForExpansion", function()
  local root = ATT:GetDataCache()
  if not (root and root.g) then TP(expansionID, root, root.g); return {} end
  local out = {}

  local function scanContainer(cat)
    if type(cat.g) ~= "table" then return end
    for _, exp in pairs(cat.g) do
      if type(exp.g) == "table" then
        for _, inst in pairs(exp.g) do
          if inst and inst.instanceID then
            local buckets = BuildEraBuckets(inst)
            -- is this instance era-split? (more than one bucket)
            local first = next(buckets)
            local isSplit = bool(first and next(buckets, first))

            for era, diffs in pairs(buckets) do
              if era == expansionID then
                local wrap = MakeEraWrapper(inst, era, diffs, isSplit)
                local removed = Util.IsNodeRemoved(inst)  -- instance-level gate
                out[#out+1] = {
                  name = Util.NodeDisplayName(inst),
                  instanceID = inst.instanceID,
                  mapID = inst.mapID,
                  savedInstanceID = inst.savedInstanceID,
                  icon = Util.GetNodeIcon(inst),
                  attNode = wrap,              -- era-scoped node
                  removed = removed,           -- for includeRemoved filtering
                }
              end
            end
          end
        end
      end
    end
  end

  for _, cat in pairs(root.g) do scanContainer(cat) end
  table.sort(out, function(a,b) return (a.name or "") < (b.name or "") end)
  return out
end)
end

function BuildZoneList()
local done = ATTPerf.auto("BuildZoneList")
  local root = ATT:GetDataCache()

  local zones, seen = {}, {}

  -- continent containers only; exclude instances & holiday/event categories
  local function isGoodContainer(n)
    return type(n) == "table"
       and type(n.mapID) == "number"
       and type(n.g) == "table" and next(n.g) ~= nil
       and not n.instanceID
       and not n.e and not n.isHolidayCategory and not n.eventID and not n.categoryID
       and C_Map.GetMapInfo(n.mapID).mapType == Enum.UIMapType.Continent
  end

  local function scan(t)
    for _, n in pairs(t) do
      if type(n) == "table" then
        if isGoodContainer(n) then
          local mid = n.mapID
          if not seen[mid] then
            seen[mid] = true
            zones[#zones+1] = { id = "zone_" .. mid, name = Util.NodeDisplayName(n), node = n }
          end
        end
        if type(n.g) == "table" then scan(n.g) end
      end
    end
  end

  scan(root.g)
  table.sort(zones, function(a,b) return (a.name or "") < (b.name or "") end)
done()
  return zones
end

-------------------------------------------------
-- Tooltip helpers (centralized)
-------------------------------------------------
Tooltip = Tooltip or {}

function Tooltip.CreateTooltip(frame, anchor, contentFunc)
    frame:EnableMouse(true)
    frame:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, anchor or "ANCHOR_RIGHT")
        GameTooltip:ClearLines()
        contentFunc()
        GameTooltip:Show()
    end)
    frame:SetScript("OnLeave", function(self) GameTooltip:Hide() end)
end

function Tooltip.AddHeader(title, r, g, b) GameTooltip:AddLine(title, r or 0, g or 1, b or 0) end
function Tooltip.AddLine(text, r, g, b)   GameTooltip:AddLine(text, r or 1, g or 1, b or 1) end

function Tooltip.AddInstanceLockoutTo(tooltip, data)
    local isLocked, numDown, numBosses, lockoutIndex = IsInstanceLockedOut(data)
    if isLocked then
        local reset = select(3, GetSavedInstanceInfo(lockoutIndex))
        tooltip:AddLine("|cffffd200Lockout expires in:|r " .. Util.FormatTime(reset))
        if (numBosses or 0) > 0 then
            tooltip:AddLine("Bosses:")
            for i = 1, numBosses do
                local bossName, _, isKilled = GetSavedInstanceEncounterInfo(lockoutIndex, i)
                local color = isKilled and "|cff00ff00" or "|cffff4040"
                tooltip:AddLine(string.format("%s%s|r", color, bossName or ("Boss " .. i)))
            end
        end
    else
        tooltip:AddLine("No active lockout.", 0.5, 0.5, 0.5)
    end
end

-- Append other-toons progress (same realm, skip current toon)
-- ownerNode: zone (mapID) or instance (instanceID) node used to key into the DB.
local function AddOtherToonsSection(tooltip, ownerNode, isZone)
  local mode = Util.GetOtherToonsMode()
  if mode == 0 then return end
  if mode == 1 and isZone then return end

  local _, realm, myChar = Util.EnsureProgressDB()
  local realmBucket = ATTGoGoDB.progress[realm]
  if not ownerNode then TP(tooltip, ownerNode, isZone, realm, myChar, realmBucket); return end

  local key, bucket
  if isZone then
    bucket = "zones"
    key = ownerNode.mapID
  else
    bucket = "instances"
    key = ownerNode.instanceID
  end
  if not key then TP(tooltip, ownerNode, isZone, mode); return end

  local rows, now = {}, time()
  for charName, perChar in pairs(realmBucket) do
    if charName ~= myChar then
      local entry = perChar[bucket] and perChar[bucket][key]

      -- Mode 1: only instances with an active lockout
      if mode == 1 then
        local secs = entry and entry.lock and (entry.lock.expiresAt - now) or 0
        if secs <= 0 then entry = nil end
      end

      if entry then
        local c, t = entry[1] or 0, entry[2] or 0
        local p = (t > 0) and (c / t * 100) or 0
        local line = string.format("• %s: %d / %d (%.1f%%)", charName, c, t, p)
        if not isZone then
          local secs = (entry.lock and entry.lock.expiresAt - now) or 0
          if secs > 0 then
            line = line .. " — " .. Util.FormatTime(secs)
          end
        end
        rows[#rows+1] = line
      end
    end
  end

  if #rows > 0 then
    table.sort(rows)
    tooltip:AddLine(" ")
    tooltip:AddLine("Other characters (" .. realm .. ")", 0.9, 0.9, 0.9)
    for _, l in ipairs(rows) do
      tooltip:AddLine(l, 0.9, 0.9, 0.9, false)
    end
  end
end

-- Consolidated progress block used by the minimap tooltip
function Tooltip.AddProgress(tooltip, data, collected, total, percent, isZone, ownerNode, lockoutData)
  tooltip:AddLine(string.format("Collected: %d / %d (%.2f%%)", collected, total, percent))
  if not isZone then
    Tooltip.AddInstanceLockoutTo(tooltip, lockoutData or data or ownerNode)
  end

  Tooltip.AddMyLockouts(tooltip)
  AddOtherToonsSection(tooltip, ownerNode, isZone)
end

function Tooltip.AddContextProgressTo(tooltip)
  local node, info = Util.ResolveContextNode()

  if info.kind == "instance" then
    local curDiff = ATT.GetCurrentDifficultyID()
    local child   = Util.SelectDifficultyChild(node, curDiff) or node
    local c, t, p = Util.ATTGetProgress(child)
    tooltip:AddLine("|cffffd200" .. Util.NodeDisplayName(node) .. "|r")
    Tooltip.AddProgress(tooltip, child, c, t, p, false, node, node)
  else
    local zoneName = GetRealZoneText()
    local subZone  = GetSubZoneText()
    local zoneDisplay = (subZone and subZone ~= "" and subZone ~= zoneName) and (subZone .. ", " .. zoneName) or zoneName
    tooltip:AddLine("|cffffd200" .. zoneDisplay .. "|r")

    local c, t, p = Util.ResolveMapProgress(info.uiMapID)
    if t <= 0 then
      tooltip:AddLine("Nothing to show for this location.", 0.7, 0.7, 0.7)
      return
    end
    -- owner stub only needs mapID for “other toons” section
    Tooltip.AddProgress(tooltip, nil, c, t, p, true, { mapID = info.uiMapID }, nil)
  end
end

function Tooltip.AddMyLockouts(tooltip)
  local me = Util.EnsureProgressDB()
  local rows, now = {}, time()
  local instances = me.instances

  for id, entry in pairs(instances) do
    if type(id) == "number" then   -- only numeric instanceID rows
      local lock = entry.lock
      if lock and (lock.expiresAt - now) > 0 then
        local total = (lock.bosses and #lock.bosses) or TP(lock) or 0
        local down = 0; for i = 1, total do if lock.bosses[i].down then down = down + 1 end end
        local node = Util.ATTSearchOne("instanceID", id)
        local name = Util.NodeDisplayName(node)
        local c, t, p = entry[1], entry[2], 0
        if t then
          p = (t > 0) and (c / t * 100) or 0
        else
          TP(id, entry[1], entry[2])
          c, t, p = Util.ATTGetProgress(node)
        end
--        if c == nil then print("id, name, c, t = ", id, name, entry[1], entry[2]) end
        local hex = CompletionHex(p, 6.7)

        rows[#rows+1] = string.format("• %s%s (%d/%d) — %d/%d (%.1f%%)|r", hex, name, down, total, c, t, p)
      end
    end
  end

  if #rows > 0 then
    table.sort(rows)
    tooltip:AddLine(" ")
    tooltip:AddLine("Locked instances:", 0.9, 0.9, 0.9)
    for _, line in ipairs(rows) do tooltip:AddLine(line, 1, 1, 1, false) end
  end
end
