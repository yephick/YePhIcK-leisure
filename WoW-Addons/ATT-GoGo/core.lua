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

function Util.FormatTime(seconds)
  local days    = math.floor(seconds / 86400)
  local hours   = math.floor((seconds % 86400) / 3600)
  local minutes = math.floor((seconds % 3600) / 60)
  return (days > 0 and (days .. "d ") or "") .. ((days > 0 or hours > 0) and (hours .. "h ") or "") .. minutes .. "m"
end

function Util.InsertNodeChatLink(node)
  local link
  if     node.itemID        then link = select(2, GetItemInfo(node.itemID))
  elseif node.achievementID then link = GetAchievementLink(node.achievementID)
  elseif node.spellID       then link = GetSpellLink(node.spellID)
  elseif node.questID       then link = GetQuestLink(node.questID)
  end
  if link and not ChatEdit_InsertLink(link) then ChatFrame_OpenChat(link) end
end

function Util.NodeDisplayName(n) return n.text or n.name or ATT.GetNameFromProviders(n) end

local _ATT_ONE_CACHE = setmetatable({}, { __mode = "v" })  -- weak values

function Util.ClearATTSearchCache() wipe(_ATT_ONE_CACHE) end

function Util.ATTSearchOne(field, id)
  local k = field .. ":" .. id
  local hit = _ATT_ONE_CACHE[k]
  if hit ~= nil then return hit end
  hit = ATT.SearchForObject(field, id, "field")  -- strict search
     or ATT.SearchForObject(field, id)           -- less strict alternative
     or (ATT.SearchForField(field, id))[1]       -- least strict fallback
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
  if hit then return hit end
  local pkg = ATT.GetCachedDataForMapID(mapID)
  local info = C_Map.GetMapInfo(mapID)
  local name = (info and info.name) or ("Map " .. mapID)
  local kids = pkg.g or pkg
  local root = { text = name, name = name, mapID = mapID, g = kids }
  _MAP_ROOT_CACHE[mapID] = root
  return root
end

-- Progress straight from the map package (matches /attmini totals)
function Util.ResolveMapProgress(mapID)
  local hit = _MAP_PROG_CACHE[mapID]
  if hit then return hit[1], hit[2], hit[3] end
  local root = Util.GetMapRoot(mapID)
  local c, t, p = Util.ATTGetProgress(root)
  _MAP_PROG_CACHE[mapID] = {c, t, p}
  return c, t, p
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
  if not node then for k in pairs(_PROG_CACHE) do _PROG_CACHE[k] = nil end; return end -- no arg => nuke all (use for BIG wave / full rebuild)
  -- targeted: clear this node and bubble to parents so rollups recompute
  local p = node
  while type(p) == "table" do _PROG_CACHE[p] = nil; p = rawget(p, "parent") end
end

function Util.ATTGetProgress(node)
  local hit = _PROG_CACHE[node]
  if hit then return hit[1], hit[2], hit[3] end

  if node.collectible then
    local c = node.collected and 1 or 0
    local t = 1
    local p = c * 100
    _PROG_CACHE[node] = { c, t, p }
    return c, t, p
  end

  -- Precomputed totals (ATT containers sometimes carry these)
  if node.total and node.total > 0 then
    local c, t = node.progress or 0, node.total
    local p = (c / t) * 100
    _PROG_CACHE[node] = { c, t, p }
    return c, t, p
  end

  local perf_cache_miss = AGGPerf.auto("Util.ATTGetProgress: cache miss")
  -- Roll up children
  local ac, at = 0, 0
  for _, ch in pairs(node.g or {}) do
    local c1, t1 = Util.ATTGetProgress(ch)
    ac, at = ac + (c1 or 0), at + (t1 or 0)
  end

  local ap = (at > 0) and (ac / at * 100) or 0
  _PROG_CACHE[node] = { ac, at, ap }

  perf_cache_miss()
  return ac, at, ap
end

function Util.GetCollectionProgress(dataset)
  local c, t = 0, 0

  for _, entry in pairs(dataset) do
    local node = (type(entry) == "table" and (entry.attNode or entry)) or nil
    local c1, t1 = Util.ATTGetProgress(node)
    c, t = c + (c1 or 0), t + (t1 or 0)
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
  if IsModifiedClick("CHATLINK") then
    ChatEdit_InsertLink(GetAchievementLink(achievementID))
    return
  end

  UIParentLoadAddOn("Blizzard_AchievementUI")
  OpenAchievementFrameToAchievement(achievementID)
  ShowUIPanel(AchievementFrame)
  AchievementFrame_SelectAchievement(achievementID)
end

-- Given a node (often a Title leaf), try to find the achievement that awards it.
function Util.FindAchievementForTitleNode(node)
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
  if node.titleID then
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
      return mapID, x, y
    end
  end

  return nil
end

function Util.TryTomTomWaypoint(mapID, x, y, title)
  if TomTom and TomTom.AddWaypoint then
    TomTom:AddWaypoint(mapID, x, y, { title = title, persistent = false })
  end
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

  if not mapID then return end

  ShowUIPanel(WorldMapFrame); WorldMapFrame:SetMapID(mapID) -- open WorldMap for the `mapID`'s zone
  _G.ATTGoGoUncollectedPopup:Show() -- make sure to re-open our uncollected popup which was auto-closed by the WorldMap

  if x and y then Util.TryTomTomWaypoint(mapID, x, y, node.text or node.name or (TITLE .. " waypoint")) end
end

function Util.GetNodeIcon(node)
  local ret = node.icon
      or ATT.GetIconFromProviders(node)
      or ATT.GetRelativeValue(node, "icon")
      or node.mapID and Util.ATTSearchOne("mapID", node.mapID).icon
      or node.achievementID and Util.ATTSearchOne("achievementID", node.achievementID).icon

  if ret then return ret end

  -- Fallback: scan ONLY this node's fields for "*ID" and ask ATT for an icon
  TP(node)
  for field, id in pairs(node) do
    if id ~= nil and type(field) == "string" and field:sub(-2) == "ID" then
      local res = Util.ATTSearchOne(field, id)
      if res.icon and res.icon ~= 0 and res.icon ~= "" then TP(field, id); return res.icon end
    end
  end

  TP(node, node.parent)

  return nil
end

-- Centralized icon applier: works with ItemButtons *and* raw Textures.
-- Usage:
--   Util.ApplyNodeIcon(btnOrTexture, node)
--   Util.ApplyNodeIcon(btnOrTexture, node, { texCoord = {0.07,0.93,0.07,0.93} })
function Util.ApplyNodeIcon(target, node, opts)
  opts = opts or {}
  local tex = opts.icon or Util.GetNodeIcon(node)   -- may be file path, fileID, atlas, or a table {atlas=..., coords=..., id=..., texture=...}
  local icon
  -- Determine the "icon" subtexture if target is an ItemButton; otherwise treat target as the Texture itself.
  if target and target.GetObjectType and target:GetObjectType() == "Texture" then
    icon = target
  else
    icon = (target and (target.icon or target.Icon or target.IconTexture))
        or (target and target.GetName and _G[target:GetName() .. "IconTexture"])
  end

  if icon then
    icon:SetTexture(tex)
    local tc = opts.texCoord or { 0, 1, 0, 1 }
    icon:SetTexCoord(tc[1], tc[2], tc[3], tc[4])
    return
  end

  -- Fallback for ItemButtons
  SetItemButtonTexture(target, tex)

end

-- === Removed/retired detection ===
local BUILD_NO = select(4, GetBuildInfo())

-- return true if a node should be considered 'removed from game' relative to current client
function Util.IsNodeRemoved(n)
  if n.u == 2 then return true end           -- ATT unobtainable flag for removed content
  if n.rwp then return n.rwp <= BUILD_NO end -- rwp: removed with patch
  if n.awp then return n.awp > BUILD_NO end  -- awp: added with patch
  return false
end

-- === Era helpers ===
local function EraFromAwp(awp)
  if not awp then return nil end
  local era = math.floor(awp / 10000)
  if era <= 0 then return 1 end
  if era >= 11 then return nil end
  return era
end

-- Derive era from a difficulty's required level when awp is missing
local function EraFromLevel(lvl)
  if type(lvl) ~= "number" then return nil end
  if     lvl <= 60 then return 1       -- Classic
  elseif lvl <= 70 then return 2   -- TBC
  elseif lvl <= 80 then return 3   -- Wrath
  elseif lvl <= 85 then return 4   -- Cata
  elseif lvl <= 90 then return 5   -- MoP
  end
  return nil
end

-- Return era for a difficulty child (prefer child.awp, then instance.awp, then instance.expansionID, else Classic)
local function EraForChild(instanceNode, child)
  return EraFromAwp(child.awp)
      or EraFromAwp(instanceNode.awp)
      or EraFromLevel(child.lvl)
      or instanceNode.expansionID
      or 1
end

-- Build { [era] = {difficultyChildren...} } ignoring non-difficulty headers
local function BuildEraBuckets(instanceNode)
  local buckets, hasDiff = {}, false
  local kids = instanceNode.g
  for _, ch in pairs(kids) do
    if ch.difficultyID then
      hasDiff = true
      local era = EraForChild(instanceNode, ch)
      local t = buckets[era] or {}
      t[#t+1] = ch
      buckets[era] = t
    end
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
    g = (diffs and #diffs>0) and diffs or instanceNode.g,
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
  for _, child in ipairs(instanceNode.g) do
    if child.difficultyID == difficultyID then
        return child
    end
  end

  return instanceNode
end

-- Returns the instance container for a given map by walking hits up to an ancestor with instanceID.
local function FindInstanceFromMap(mapID)
  local pick
  local function try(field)
      local hits = ATT.SearchForField(field, mapID)
      if type(hits) ~= "table" then return end
      for i = 1, #hits do
      local p = hits[i]
      while p and not p.instanceID do p = p.parent end
      if p and p.instanceID then
          pick = p
          return
      end
      end
  end
  -- prefer 'maps' matches (instances commonly use it), then 'mapID'
  try("maps")
  if not pick then try("mapID") end
  return pick
end

-- Unified context resolver: returns the ATT node for current instance or zone.
-- Returns: node, info  where info={kind="instance"|"zone", uiMapID=?}
function Util.ResolveContextNode()
  local info = { uiMapID = ATT.CurrentMapID }
  local node = ATT.GetCachedDataForMapID(info.uiMapID)

  if IsInInstance() then
    info.kind = "instance"
    local _, instType = GetInstanceInfo()
    if instType == "party" or instType == "raid" then
      -- narrow to the current difficulty
      local curDiff = ATT.GetCurrentDifficultyID()
      local child   = Util.SelectDifficultyChild(node, curDiff) or node  -- returns diff child, or the node itself. 
    
      -- determine current era + whether the instance is era-split, then wrap
      local buckets = BuildEraBuckets(node)                               -- { [era] = {diff-children...} } 
      local first   = next(buckets)
      local isSplit = first and next(buckets, first)
      local era     = EraForChild(node, child) or first or node.expansionID or 1
      node          = MakeEraWrapper(node, era, { child }, isSplit)
    end
  else
    info.kind = "zone"
  end

  return node or TP(info), info
end

function Util.ResolvePopupTargetForCurrentContext()
  local node, info = Util.ResolveContextNode()
  if info.kind == "instance" then
    return node
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
  for i = 1, GetNumSavedInstances() do
    local _, _, _, _, locked, _, _, _, _, _, numEncounters, numCompleted, _, savedInstanceID = GetSavedInstanceInfo(i)
    if locked and savedInstanceID == sid then
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

  local realm = GetRealmName()
  prog[realm] = prog[realm] or {}

  local charName = UnitName("player")
  local byChar = prog[realm]
  -- always reset this toon’s layout (new schema every load)
  byChar[charName] = {
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
    bosses[#bosses+1] = { name = bossName, down = killed }
  end

  return { expiresAt = expiresAt, bosses = bosses, sid = sid }
end

function Util.SaveInstanceProgressByNode(attInstanceNode)
  local instanceID = attInstanceNode.instanceID

  local key = Util.GetInstanceProgressKey(attInstanceNode)
  local c, t = Util.ATTGetProgress(attInstanceNode)
  local lock = BuildLockoutFromSavedInstances(attInstanceNode)

  local me = Util.EnsureProgressDB()
  me.instances = me.instances or {}

  me.instances[instanceID] = { c, t, lock = lock } -- split and non-split: progress + lock under numeric key
  if key ~= instanceID then
    me.instances[key] = { c, t } -- split: store progress under composite key
  end
end

-- Save zone progress for an ATT zone node
function Util.SaveZoneProgressByMapID(mapID)
  local c, t = Util.ResolveMapProgress(mapID)
  Util.EnsureProgressDB().zones[mapID] = { c or 0, t or 0 }
end

-- Convenience: snapshot whatever the current context is
function Util.SaveCurrentContextProgress()
  local node, info = Util.ResolveContextNode()

  if info.kind == "instance" then
    Util.SaveInstanceProgressByNode(node)
  else
    Util.SaveZoneProgressByMapID(info.uiMapID)
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
  return ("|cff%02x%02x%02x"):format(r*255, g*255, b*255)
end

function BuildExpansionList()
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
          list[#list+1] = { id = id, name = exp.text, node = exp }
        end
      end
    end
  end

  for _, cat in pairs(root.g) do scanContainer(cat) end
  table.sort(list, function(a,b) return a.id < b.id end)
  return list
end

function Util.GetInstanceProgressKey(node)
  if node.progressKey ~= nil then return node.progressKey end
  local id = node.instanceID; if not id then return nil end -- may happen when TP'ing out and back into an LFG instance (happened to me in East DM after 2 people left the group)
  local era = node.eraKey
  -- if we don’t know whether it’s split, default to legacy (numeric)
  node.progressKey = (node.__eraSplit and era) and (id .. ":" .. era) or id
  return node.progressKey
end

function GetInstancesForExpansion(expansionID)
  local root = ATT:GetDataCache()
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
            local isSplit = first and next(buckets, first)

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
end

function BuildZoneList()
  local root = ATT:GetDataCache()

  local zones, seen = {}, {}

  -- continent containers only
  local function isGoodContainer(n)
    return type(n) == "table"
       and type(n.mapID) == "number"
       and type(n.g) == "table" and next(n.g) ~= nil
       and not n.instanceID
       and C_Map.GetMapInfo(n.mapID).mapType == Enum.UIMapType.Continent
  end

  local depth = 0 -- "Continents" are right under "Outdoor zones" and no deeper than that
  local function scan(t)
    if depth > 2 then return end
    depth = depth + 1
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
    depth = depth - 1
  end

  scan(root.g)
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

function Tooltip.AddHeader(title) GameTooltip:AddLine(title, 0, 1, 0) end
function Tooltip.AddLine  (text)  GameTooltip:AddLine(text,  1, 1, 1) end

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
                tooltip:AddLine(("%s%s|r"):format(color, bossName or ("Boss " .. i)))
            end
        end
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

  local key, bucket
  if isZone then
    bucket = "zones"
    key = ownerNode.mapID
  else
    bucket = "instances"
    key = ownerNode.instanceID
  end

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
        local line = ("• %s: %d / %d (%.1f%%)"):format(charName, c, t, p)
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
  tooltip:AddLine(("Collected: %d / %d (%.2f%%)"):format(collected, total, percent))
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
    Tooltip.AddProgress(tooltip, child, c, t, p, false, node, child)
  else
    local zoneName = GetRealZoneText()
    local subZone  = GetSubZoneText()
    local zoneDisplay = (subZone and subZone ~= "" and subZone ~= zoneName) and (subZone .. ", " .. zoneName) or zoneName
    tooltip:AddLine("|cffffd200" .. zoneDisplay .. "|r")

    local c, t, p = Util.ResolveMapProgress(info.uiMapID)
    if t > 0 then
      -- owner stub only needs mapID for “other toons” section
      Tooltip.AddProgress(tooltip, nil, c, t, p, true, { mapID = info.uiMapID }, nil)
    end
  end
end

function Tooltip.AddMyLockouts(tooltip)
  local me = Util.EnsureProgressDB()
  local rows, now = {}, time()

  for id, entry in pairs(me.instances) do
    if type(id) == "number" then   -- only numeric instanceID rows
      local lock = entry.lock
      if lock and (lock.expiresAt - now) > 0 then
        local total = #lock.bosses
        local down = 0; for i = 1, total do if lock.bosses[i].down then down = down + 1 end end
        local bossTxt = (total > 0 and ("(%d/%d) "):format(down, total) or "")
        local node = Util.ATTSearchOne("instanceID", id)
        local name = Util.NodeDisplayName(node)
        local c, t, p = entry[1], entry[2], 0
        p = (t > 0) and (c / t * 100) or 0
        local hex = CompletionHex(p, 6.7)
        rows[#rows+1] = ("• %s%s %s— %d/%d (%.1f%%)|r"):format(hex, name, bossTxt, c, t, p)
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
