-- === ATT-GoGo Utility Functions ===

-- all the global variables/tables/etc are in this file to ensure smooth access with no order dependencies

-- SavedVariables tables
ATTGoGoDB = ATTGoGoDB or {}
ATTGoGoCharDB = ATTGoGoCharDB or {}

ATT = nil -- this is our reference to ATT addon on which we rely

COLLECTIBLE_ID_FIELDS = {
    achievementID = true,
    creatureID = false,
    explorationID = true,
    flightpathID = true,
    gearSetID = false,
    instanceID = false,
    itemID = false,
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
    gearSetID = "gear set",
    instanceID = "instance",
    itemID = "item",
    mapID = "map",
    questID = "quest",
    titleID = "title",
    visualID = "visual",
}

Util = Util or {}

-- === Settings helpers (account/character) ===
ATTGoGoDB     = ATTGoGoDB     or {}
ATTGoGoCharDB = ATTGoGoCharDB or {}

--function Util.WithStopwatch(label, fn, ...)
--    local t0 = (debugprofilestop and debugprofilestop()) or (GetTimePreciseSec() * 1000)
--    local ok, a, b, c = pcall(fn, ...)
--    local t1 = (debugprofilestop and debugprofilestop()) or (GetTimePreciseSec() * 1000)
----    DebugLogf("[Perf] %s: %.2f ms", label, t1 - t0)
--    if not ok then error(a) end
--    return a, b, c
--end

-- === Account-scoped settings ===
function GetSetting(key, default)
    ATTGoGoDB = ATTGoGoDB or {}
    local v = ATTGoGoDB[key]
    if v == nil then return default end
    return v
end

function SetSetting(key, value)
    ATTGoGoDB = ATTGoGoDB or {}
    ATTGoGoDB[key] = value
end

-- === Character-scoped settings ===
function GetCharSetting(key, default)
  ATTGoGoCharDB = ATTGoGoCharDB or {}
  local v = ATTGoGoCharDB[key]
  if v == nil then return default end
  return v
end

function SetCharSetting(key, value)
  ATTGoGoCharDB = ATTGoGoCharDB or {}
  ATTGoGoCharDB[key] = value
end

-------------------------------------------------
-- Progress cache (weak-key, cleared on ATT refresh)
-------------------------------------------------
local ProgressCache = setmetatable({}, { __mode = "k" })

function Util.ClearProgressCache()
  for k in pairs(ProgressCache) do ProgressCache[k] = nil end
end

local function cachedProgress(node)
  local e = ProgressCache[node]
  if e then return e.c, e.t, e.p end
  local c, t, p = Util.ATTGetProgress(node)
  ProgressCache[node] = { c = c, t = t, p = p }
  return c, t, p
end

-------------------------------------------------
-- Small helpers
-------------------------------------------------
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

function Util.FormatLockoutTime(seconds)
  local days    = math.floor(seconds / 86400)
  local hours   = math.floor((seconds % 86400) / 3600)
  local minutes = math.floor((seconds % 3600) / 60)
  return (days > 0 and (days .. "d ") or "") .. (hours > 0 and (hours .. "h ") or "") .. minutes .. "m"
end

function Util.InsertNodeChatLink(node)
  if not node then return end
  local link
  if node.itemID then link = select(2, GetItemInfo(node.itemID))
  elseif node.achievementID then link = GetAchievementLink(node.achievementID)
  elseif node.spellID then link = GetSpellLink(node.spellID)
  elseif node.questID then link = GetQuestLink(node.questID) end
  if link then
    if not ChatEdit_GetActiveWindow() then ChatFrame_OpenChat("") end
    ChatEdit_InsertLink(link)
  end
end

function Util.NodeDisplayName(n)
  if not n or type(n) ~= "table" then return "?" end
  return n.text or n.name
      or (n.mapID and ("Map " .. tostring(n.mapID)))
      or (n.instanceID and ("Instance " .. tostring(n.instanceID)))
      or "?"
end

-------------------------------------------------
-- Fast ATT search wrappers
-------------------------------------------------
local function _ATT()
  return _G.ATTC or _G.AllTheThings
end

function Util.ATTSearchOne(field, id)
  return AllTheThings.SearchForField(field, id)[1] or AllTheThings.SearchForObject(field, id, "field", false)
end

-------------------------------------------------
-- Progress resolution
-------------------------------------------------
function Util.ATTGetProgress(node)
  if not node then return 0, 0, 0 end
  if node.collectible then
    return node.collected and 1 or 0, 1, node.collected and 100 or 0
  end
  local c = node.progress or 0
  local t = node.total or 0
  if t > 0 then return c, t, (c / t) * 100 end
  if node.g and #node.g > 0 then
    local ac, at = 0, 0
    for _, child in ipairs(node.g) do
      local c1, t1 = Util.ATTGetProgress(child)
      ac, at = ac + c1, at + t1
    end
    if at > 0 then return ac, at, (ac / at) * 100 end
  end
  return 0, 0, 0
end

function Util.ResolveBestProgressNode(node)
  if type(node) ~= "table" then return node, 0, 0, 0 end
  local c, t, p = cachedProgress(node)
  if t and t > 0 then return node, c, t, p end

  local best, bc, bt
  if type(node.g) == "table" then
    for i = 1, #node.g do
      local ch = node.g[i]
      if type(ch) == "table" then
        local c1, t1 = cachedProgress(ch)
        if (t1 or 0) > 0 and ((not bt) or t1 > bt) then
          best, bc, bt = ch, c1, t1
        end
      end
    end
  end
  if best then return best, bc, bt, (bc / bt) * 100 end
  return node, 0, 0, 0
end

function Util.ResolveProgress(node)
  local e = ProgressCache[node]
  if e then return e.c, e.t, e.p end
  local c, t, p = Util.ATTGetProgress(node)
  ProgressCache[node] = { c = c, t = t, p = p }
  return c, t, p
end

function Util.GetCollectionProgress(dataset)
  local c, t = 0, 0
  if type(dataset) ~= "table" then return 0, 0, 0 end
  for _, entry in ipairs(dataset) do
    local node = (type(entry) == "table" and (entry.attNode or entry)) or nil
    if node then
      local c1, t1 = cachedProgress(node)
      c, t = c + (c1 or 0), t + (t1 or 0)
    end
  end
  return c, t, (t > 0) and (c / t * 100) or 0
end

-------------------------------------------------
-- Frame pos helpers
-------------------------------------------------
-- Save frame position (+size) to DB
function Util.SaveFramePosition(frame, dbKey)
    if not (frame and dbKey) then return end
    local point, _, relativePoint, xOfs, yOfs = frame:GetPoint()
    local w, h = frame:GetSize()
    local DB = ATTGoGoCharDB or {}

    DB[dbKey] = DB[dbKey] or {}
    local rec = DB[dbKey]
    rec.point = point
    rec.relativePoint = relativePoint
    rec.xOfs = xOfs
    rec.yOfs = yOfs
    if w and h then
        rec.width  = math.floor(w + 0.5)
        rec.height = math.floor(h + 0.5)
    end
end

-- Load frame position (+size) from DB with defaults
function Util.LoadFramePosition(frame, dbKey, defaultPoint, defaultX, defaultY)
    local DB  = ATTGoGoCharDB
    local pos = DB and DB[dbKey]
    frame:ClearAllPoints()
    if pos then
        frame:SetPoint(pos.point or "CENTER", UIParent, pos.relativePoint or "CENTER", pos.xOfs or 0, pos.yOfs or 0)
        if pos.width and pos.height then
            frame:SetSize(pos.width, pos.height)
        end
    else
        frame:SetPoint(defaultPoint or "CENTER", defaultX or 0, defaultY or 0)
    end
end

function Util.ClearChildrenOrTabs(arg)
  if arg and type(arg) ~= "string" and arg.GetChildren then
    for _, child in ipairs({ arg:GetChildren() }) do
      if child.Hide then child:Hide() end
      if child.SetParent then child:SetParent(nil) end
    end
    return
  end
  if type(arg) == "table" then
    for k, v in pairs(arg) do
      if type(v) == "table" and v.Hide and v.SetParent then v:Hide(); v:SetParent(nil) end
      arg[k] = nil
    end
    wipe(arg)
  end
end

function Util.GetGridCols(scrollWidth, widgetSize, padding)
  local cols = math.floor((scrollWidth + padding) / (widgetSize + padding))
  return (cols < 1) and 1 or cols
end

function Util.SetTooltip(frame, anchor, title, ...)
  local lines = { ... }
  Tooltip.CreateTooltip(frame, anchor, function()
    Tooltip.AddHeader(title)
    for _, line in ipairs(lines) do Tooltip.AddLine(line) end
  end)
end

-- Per-char popup id-filters (merge defaults)
function Util.GetPopupIdFilters()
  ATTGoGoCharDB = ATTGoGoCharDB or {}
  local t = ATTGoGoCharDB.popupIdFilters
  if type(t) ~= "table" then t = {}; ATTGoGoCharDB.popupIdFilters = t end

  -- keep only known keys
  for k in pairs(t) do
    if COLLECTIBLE_ID_FIELDS[k] == nil then t[k] = nil end
  end
  -- merge defaults
  for key, default in pairs(COLLECTIBLE_ID_FIELDS) do
    if t[key] == nil then t[key] = default and true or false end
  end
  return t
end

function Util.SetPopupIdFilter(key, value)
  local t = Util.GetPopupIdFilters()
  t[key] = value and true or false
end

-------------------------------------------------
-- Achievement helpers
-------------------------------------------------
function Util.OpenAchievementByID(achievementID)
  if not achievementID then return end

  if IsModifiedClick("CHATLINK") then
    local link = GetAchievementLink(achievementID)
    if link then ChatEdit_InsertLink(link) return end
  end

  if not IsAddOnLoaded("Blizzard_AchievementUI") then
    UIParentLoadAddOn("Blizzard_AchievementUI")
  end
  if OpenAchievementFrameToAchievement then
    OpenAchievementFrameToAchievement(achievementID)
    return
  end
  if AchievementFrame then
    ShowUIPanel(AchievementFrame)
    if AchievementFrame_SelectAchievement then
      AchievementFrame_SelectAchievement(achievementID)
    end
  end
end

function Util.GetBestAchievementID(node)
  if not node then return nil end
  local function firstUncollectedLeafWithAch(n)
    if type(n) ~= "table" then return nil end
    local ach, hasKids = n.achievementID, (n.g and #n.g > 0)
    if ach and not hasKids and n.collected ~= true then return ach end
    if n.g then for i = 1, #n.g do local id = firstUncollectedLeafWithAch(n.g[i]); if id then return id end end end
    if ach and n.collected ~= true then return ach end
    return nil
  end
  return firstUncollectedLeafWithAch(node) or node.achievementID
end

-------------------------------------------------
-- Map/waypoint helpers
-------------------------------------------------
function Util.ExtractMapAndCoords(node)
  if not node or type(node) ~= "table" then return nil end
  local function isValidMap(id)
    return type(id) == "number" and id >= 1 and id % 1 == 0
       and C_Map and C_Map.GetMapInfo and C_Map.GetMapInfo(id) ~= nil
  end
  local function normXY(x, y)
    if type(x) ~= "number" or type(y) ~= "number" then return nil, nil end
    if x > 1 then x = x / 100 end
    if y > 1 then y = y / 100 end
    x = math.max(0, math.min(1, x)); y = math.max(0, math.min(1, y))
    return x, y
  end

  local raw = node.mapID
  local mapID = isValidMap(raw) and raw or nil

  local function parseTriple(t)
    if type(t) ~= "table" then return nil end
    local a, b, c = t[1], t[2], t[3]
    local aIsMap, cIsMap = isValidMap(a), isValidMap(c)
    if cIsMap then
      if (not mapID) or (c == mapID) then local x,y=normXY(a,b); if x and y then return c,x,y end end
    end
    if aIsMap and ((not mapID) or (a == mapID)) then
      local x,y=normXY(b,c); if x and y then return a,x,y end
    end
    if aIsMap and cIsMap then
      local x3,y3=normXY(a,b); if x3 and y3 then return c,x3,y3 end
    end
    local x,y=normXY(a,b); if x and y then return nil,x,y end
    return nil
  end

  if type(node.coord) == "table" then
    local m, x, y = parseTriple(node.coord); mapID = isValidMap(m) and m or mapID
    if mapID and x and y then return mapID, x, y end
  end
  if type(node.coords) == "table" and #node.coords > 0 then
    local m, x, y = parseTriple(node.coords[1]); mapID = isValidMap(m) and m or mapID
    if mapID and x and y then return mapID, x, y end
  end
  if mapID then return mapID, nil, nil end
  return nil
end

function Util.OpenWorldMapTo(mapID)
  if not (mapID and C_Map and C_Map.GetMapInfo and C_Map.GetMapInfo(mapID)) then return end
  if WorldMapFrame then
    if not WorldMapFrame:IsShown() then if ShowUIPanel then ShowUIPanel(WorldMapFrame) else WorldMapFrame:Show() end end
    if WorldMapFrame.SetMapID then WorldMapFrame:SetMapID(mapID)
    elseif SetMapByID then SetMapByID(mapID) end
  elseif OpenWorldMap then
    OpenWorldMap(mapID)
  end
end

do
  local overlay
  function Util.HighlightWorldMapPulse()
    if not WorldMapFrame then return end
    if not overlay then
      overlay = CreateFrame("Frame", nil, WorldMapFrame)
      overlay:SetAllPoints(WorldMapFrame)
      overlay.tex = overlay:CreateTexture(nil, "OVERLAY")
      overlay.tex:SetAllPoints(overlay)
      overlay.tex:SetColorTexture(0, 1, 1, 0.20)
      overlay:Hide()
    end
    overlay:Show()
    overlay:SetAlpha(0.0)
    local t, dur, dir = 0, 0.8, 1
    overlay:SetScript("OnUpdate", function(self, elapsed)
      t = t + elapsed * dir
      if t >= dur then dir = -1 end
      if t <= 0 then self:SetScript("OnUpdate", nil); self:Hide(); return end
      self:SetAlpha((t/dur) * 0.55)
    end)
  end
end

function Util.TryTomTomWaypoint(mapID, x, y, title)
  if not (mapID and C_Map and C_Map.GetMapInfo and C_Map.GetMapInfo(mapID)) then return false end
  if not (type(x)=="number" and type(y)=="number") then return false end
  title = title or "ATT-GoGo"
  if TomTom and TomTom.AddWaypoint then
    pcall(TomTom.AddWaypoint, TomTom, mapID, x, y, { title = title, persistent = false })
    return true
  end
  return false
end

function Util.FocusMapForNode(node)
  if not node then return false end
  local mapID, x, y = Util.ExtractMapAndCoords(node)
  if not mapID and node.instanceID then
    local inst = Util.ATTFindInstanceByInstanceID(node.instanceID)
    if inst then mapID, x, y = Util.ExtractMapAndCoords(inst) end
  end
  if not mapID and node.flightpathID and node.g then
    for _, ch in ipairs(node.g) do mapID, x, y = Util.ExtractMapAndCoords(ch); if mapID then break end end
  end
  if not mapID then return false end
  Util.OpenWorldMapTo(mapID)
  C_Timer.After(0, Util.HighlightWorldMapPulse)
  if x and y then Util.TryTomTomWaypoint(mapID, x, y, node.text or node.name or "Waypoint") end
  return true
end

function Util.GetNodeIcon(node)
  if not node then return nil end
  if node.icon then return node.icon end

  -- meta-achievement icon via Blizzard API (covers our stub reps)
  if node.achievementID and GetAchievementInfo then
    local _, _, _, _, _, _, _, _, _, icon = GetAchievementInfo(node.achievementID)
    if icon then return icon end
  end

  -- spell icons
  if node.spellID and GetSpellTexture then
    local icon = GetSpellTexture(node.spellID)
    if icon then return icon end
  end

  -- bubble up a few parents if needed (ATT may populate later for items)
  local p, hops = rawget(node, "parent"), 0
  while p and hops < 5 do
    if p.icon then return p.icon end
    p = rawget(p, "parent"); hops = hops + 1
  end
  return nil
end

-- === Removed/retired detection ===
-- Convert "major.minor.patch" into ATT-style RWP integer (e.g. "5.5.0" -> 50500, "1.15.3" -> 11503)
function Util.CurrentClientRWP()
  if not GetBuildInfo then return nil end
  local ver = (GetBuildInfo())
  if type(ver) ~= "string" then return nil end
  local maj, min, pat = ver:match("^(%d+)%.(%d+)%.?(%d*)")
  maj, min, pat = tonumber(maj), tonumber(min), tonumber(pat) or 0
  if not (maj and min) then return nil end
  return (maj * 10000) + (min * 100) + pat
end

-- Return true if a node should be considered 'removed from game' relative to current client.
-- Heuristics:
--   - ATT nodes often carry 'rwp' (removed-with-patch) as ATT-style int.
--   - Some nodes carry unobtainable flag 'u == 2' in ATT, treat as removed.
--   - If neither is present, treat as not removed.
function Util.IsNodeRemoved(n, nowRWP)
  if type(n) ~= "table" then return false end
  nowRWP = nowRWP or Util.CurrentClientRWP()

  -- ATT unobtainable flag for removed content
  if n.u == 2 then return true end

  -- rwp: removed with patch <= client build
  local r = tonumber(n.rwp)
  if r and nowRWP then
    return r <= nowRWP
  end

  return false
end

-------------------------------------------------
-- ATT instance resolvers & zone helper
-------------------------------------------------
local function _Root()
  local api = _ATT()
  if not api or type(api.GetDataCache) ~= "function" then return nil end
  return api:GetDataCache()
end

ATTDB = ATTDB or {}

function ATTDB.BuildInstanceCache()
--  DebugLogf("[Trace] ATTDB_BuildInstanceCache")
  local cache = { list = {}, byMapID = {}, byInstanceID = {}, byEJID = {} }

  local exps = BuildExpansionList()
  for _, exp in ipairs(exps) do
    local insts = GetInstancesForExpansion(exp.id)
    for _, e in ipairs(insts) do
      local n = e.attNode or e
      if type(n) == "table" then
        cache.list[#cache.list+1] = n
        if n.mapID       then cache.byMapID[n.mapID]         = n end
        if n.instanceID  then cache.byInstanceID[n.instanceID] = n; cache.byEJID[n.instanceID] = n end
      end
    end
  end

  ATTDB.cache = cache
--  DebugLogf("[ATTDB] cache built: instances=%d", #cache.list)
  return cache
end

function ATTDB.GetCache()
  local c = ATTDB.cache
  if not c or not c.list or #c.list == 0 then return ATTDB.BuildInstanceCache() end
  return c
end

do
  local api = _ATT()
  if api and api.AddEventHandler and not ATTDB.__wired then
    ATTDB.__wired = true
    api.AddEventHandler("OnReady", function() ATTDB.BuildInstanceCache() end)
  end
end

function Util.ATTFindInstanceByInstanceID(id)
    local api = _ATT()
    if not api then return nil end
    return api.SearchForField("instanceID", id)[1]
end

function Util.ATTFindInstanceByMapID(id)
    local api = _ATT()
    if not api then return nil end
    return api.SearchForField("mapID", id)[1]
end

-- Instance whose `maps` array contains a given uiMapID
function Util.ATTFindInstanceByContainedMap(uiMapID)
  uiMapID = tonumber(uiMapID); if not uiMapID then return nil end
  local root = _Root(); if not (root and root.g) then return nil end
  for _, cat in ipairs(root.g) do
    if type(cat.g) == "table" then
      for _, exp in ipairs(cat.g) do
        if type(exp) == "table" and type(exp.g) == "table" then
          for _, n in ipairs(exp.g) do
            if type(n) == "table" and n.instanceID and type(n.maps) == "table" then
              for _, m in ipairs(n.maps) do
                if tonumber(m) == uiMapID then return n end
              end
            end
          end
        end
      end
    end
  end
  return nil
end

-- Instance by Blizzard savedInstanceID
function Util.ATTFindInstanceBySavedInstanceID(id)
  id = tonumber(id); if not id then return nil end
  local root = _Root(); if not (root and root.g) then return nil end
  for _, cat in ipairs(root.g) do
    if type(cat.g) == "table" then
      for _, exp in ipairs(cat.g) do
        if type(exp) == "table" and type(exp.g) == "table" then
          for _, n in ipairs(exp.g) do
            if type(n) == "table" and n.instanceID and tonumber(n.savedInstanceID) == id then
              return n
            end
          end
        end
      end
    end
  end
  return nil
end

-- Unified context resolver: returns the ATT node for current instance or zone.
-- Returns: node, info  where info={kind="instance"|"zone", source="maps[]|savedInstanceID|zoneMap", uiMapID=?, giMapID=?}
function Util.ResolveContextNode(verbose)
  local info = {}
  local function ret(node, kind, source)
    info.kind, info.source = kind, source
    return verbose and node, info or node
  end

  info.uiMapID = C_Map and C_Map.GetBestMapForUnit and C_Map.GetBestMapForUnit("player") or nil
  local gi = { GetInstanceInfo() }
  info.giName  = gi[1]
  info.giMapID = tonumber(gi[8])
  local inInstance = IsInInstance()

--  DebugLogf("[Trace] ResolveContextNode(uiMapID=%s, giMapID=%s, inInst=%s)", tostring(info.uiMapID), tostring(info.giMapID), tostring(inInstance))

  -- Instance by contained uiMapID in node.maps[]
  if inInstance and info.uiMapID then
    local byContained = Util.ATTFindInstanceByContainedMap(info.uiMapID)
    if byContained then return ret(byContained, "instance", "maps[]") end
  end

  -- Instance by Blizzard savedInstanceID (Classic)
  if inInstance and info.giMapID then
    local bySaved = Util.ATTFindInstanceBySavedInstanceID(info.giMapID)
    if bySaved then return ret(bySaved, "instance", "savedInstanceID") end
  end

  -- Zone fallback by mapID
  if info.uiMapID then
    local zoneNode = ResolveBestZoneNode(info.uiMapID)
    if zoneNode then return ret(zoneNode, "zone", "zoneMap") end
  end

  return ret(nil, inInstance and "instance" or "zone", "none")
end

function ResolveBestZoneNode(mapID)
--  DebugLogf("[Trace] ResolveBestZoneNode(%s)", tostring(mapID))
  if not mapID then return nil end
  local strict = ResolveContainerZoneNodeStrict(mapID)
  if strict then return strict end
  local n = Util.ATTSearchOne("mapID", tonumber(mapID))
  local cur, safety = n, 0
  while cur and safety < 10 do
    if type(cur.g) == "table" and #cur.g > 0 and not cur.instanceID then return cur end
    cur = rawget(cur, "parent"); safety = safety + 1
  end
  return n
end

function ResolveContainerZoneNodeStrict(mapID)
--  DebugLogf("[Trace] ResolveContainerZoneNodeStrict(%s)", tostring(mapID))
  if not mapID then return nil end
  local root = _Root(); if not (root and root.g) then return nil end
  local function isContainerZone(n)
    return type(n)=="table"
       and n.mapID == mapID
       and type(n.g)=="table" and #n.g>0
       and not n.instanceID
  end
  local function scan(t)
    for _, n in ipairs(t) do
      if isContainerZone(n) then return n end
      if type(n)=="table" and type(n.g)=="table" then
        local r = scan(n.g); if r then return r end
      end
    end
  end
  return scan(root.g)
end

function IsInstanceLockedOut(instance)
  if not GetNumSavedInstances then return false end
  local sid = type(instance) == "table" and tonumber(instance.savedInstanceID) or tonumber(instance)
  if not sid then return false end
  for i = 1, GetNumSavedInstances() do
    local _, _, _, _, locked, _, _, _, _, _, numEncounters, numCompleted, _, savedInstanceID = GetSavedInstanceInfo(i)
    if locked and tonumber(savedInstanceID) == sid then
      return true, (numCompleted or 0), (numEncounters or 0), i
    end
  end
  return false
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

function BuildExpansionList()
--  DebugLogf("[Trace] BuildExpansionList()")
  local list, seen = {}, {}
  local root = _Root()
  if not (root and type(root.g) == "table") then return list end

  local function scanContainer(cat)
    if type(cat.g) ~= "table" then return end
    for _, exp in ipairs(cat.g) do
      local id = exp and exp.expansionID
      if id and type(exp.g) == "table" and not seen[id] then
        local hasInstance = false
        for _, c in ipairs(exp.g) do if c and c.instanceID then hasInstance = true break end end
        if hasInstance then
          seen[id] = true
          list[#list+1] = { id = id, name = exp.text or ("Expansion " .. tostring(id)), node = exp }
        end
      end
    end
  end

  for _, cat in ipairs(root.g) do scanContainer(cat) end
  table.sort(list, function(a,b) return (a.id or 0) < (b.id or 0) end)
  return list
end

function GetInstancesForExpansion(expansionID)
--  DebugLogf("[Trace] GetInstancesForExpansion(%s)", tostring(expansionID))
  local out = {}
  local root = _Root()
  if not (root and root.g) then return out end

  local function scanContainer(cat)
    if type(cat.g) ~= "table" then return end
    for _, exp in ipairs(cat.g) do
      if exp and exp.expansionID == expansionID and type(exp.g) == "table" then
        for _, child in ipairs(exp.g) do
          if child and child.instanceID then
            out[#out+1] = {
              name = Util.NodeDisplayName(child),
              instanceID = child.instanceID,
              mapID = child.mapID, savedInstanceID = child.savedInstanceID,
              icon = Util.GetNodeIcon(child), attNode = child,
            }
          end
        end
      end
    end
  end

  for _, cat in ipairs(root.g) do scanContainer(cat) end
  table.sort(out, function(a,b) return (a.name or "") < (b.name or "") end)
  return out
end

function BuildZoneList()
--  DebugLogf("[Trace] BuildZoneList()")
  local root = _Root(); if not (root and root.g) then return {} end

  local zones, seen = {}, {}
  local UIMapType_Continent = (Enum and Enum.UIMapType and Enum.UIMapType.Continent) or 2
  local function isContinent(mapID)
    if not (C_Map and C_Map.GetMapInfo) then return false end
    local mi = C_Map.GetMapInfo(mapID)
    return mi and mi.mapType == UIMapType_Continent
  end

  -- continent containers only; exclude instances & holiday/event categories
  local function isGoodContainer(n)
    return type(n) == "table"
       and type(n.mapID) == "number"
       and type(n.g) == "table" and #n.g > 0
       and not n.instanceID
       and not n.e and not n.isHolidayCategory and not n.eventID and not n.categoryID
       and isContinent(n.mapID)
  end

  local function scan(t)
    for _, n in ipairs(t) do
      if type(n) == "table" then
        if isGoodContainer(n) then
          local mid = n.mapID
          if not seen[mid] then
            seen[mid] = true
            zones[#zones+1] = { id = "zone_" .. tostring(mid), name = Util.NodeDisplayName(n), node = n }
--            DebugLogf("[Trace] BuildZoneList: +continent %s", tostring(mid))
          end
        end
        if type(n.g) == "table" then scan(n.g) end
      end
    end
  end

  scan(root.g)
  table.sort(zones, function(a,b) return (a.name or "") < (b.name or "") end)
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
        if contentFunc then contentFunc() end
        GameTooltip:Show()
    end)
    frame:SetScript("OnLeave", function(self) GameTooltip:Hide() end)
end

function Tooltip.AddHeader(title, r, g, b) GameTooltip:AddLine(title, r or 0, g or 1, b or 0) end
function Tooltip.AddLine(text, r, g, b)   GameTooltip:AddLine(text, r or 1, g or 1, b or 1) end

function Tooltip.AddInstanceLockoutTo(tooltip, data)
    local isLocked, numDown, numBosses, lockoutIndex = IsInstanceLockedOut(data)
    if isLocked and lockoutIndex then
    local reset = select(3, GetSavedInstanceInfo(lockoutIndex))
    local timeStr = Util.FormatLockoutTime(reset)
    tooltip:AddLine("|cffffd200Lockout expires in:|r " .. timeStr)
        tooltip:AddLine("Bosses:")
        for i = 1, (numBosses or 0) do
            local bossName, _, isKilled = GetSavedInstanceEncounterInfo(lockoutIndex, i)
      local color = isKilled and "|cff00ff00" or "|cffff4040"
      tooltip:AddLine(string.format("%s%s|r", color, bossName or ("Boss " .. i)))
        end
    else
        tooltip:AddLine("No active lockout.", 0.5, 0.5, 0.5)
    end
end

function Tooltip.AddProgress(tooltip, data, collected, total, percent, isZone, lockoutData)
  tooltip:AddLine(string.format("Collected: %d / %d (%.2f%%)", collected, total, percent))
  if not isZone then
    Tooltip.AddInstanceLockoutTo(tooltip, lockoutData or data)
  end
end

function Tooltip.AddContextProgressTo(tooltip)
  local node, info = Util.ResolveContextNode(true)
  if not node then
    tooltip:AddLine("Not in an instance.", 0.5, 0.5, 0.5)
    return
  end

  if info.kind == "instance" then
    local best, c, t, p = Util.ResolveBestProgressNode(node)
    tooltip:AddLine("|cffffd200" .. Util.NodeDisplayName(node) .. "|r")
    Tooltip.AddProgress(tooltip, best, c, t, (t > 0 and (c/t*100) or 0), false, node)
  else
    -- Always show the zone/sub-zone name…
    local zoneName = GetRealZoneText() or (node and node.text) or "Zone"
    local subZone  = GetSubZoneText()
    local zoneDisplay = (subZone and subZone ~= "" and subZone ~= zoneName) and (subZone .. ", " .. zoneName) or zoneName
    tooltip:AddLine("|cffffd200" .. zoneDisplay .. "|r")

    -- …but only show progress when the zone exists in Outdoor Zones strictly.
    local strictZone = ResolveContainerZoneNodeStrict(info.uiMapID)
    if not strictZone then
      tooltip:AddLine("Nothing to show for this location.", 0.7, 0.7, 0.7)
      return
    end

    local best, c, t, p = Util.ResolveBestProgressNode(strictZone)
    Tooltip.AddProgress(tooltip, best or strictZone or {}, c, t, (t > 0 and (c/t*100) or 0), true)
  end
end

-------------------------------------------------
-- Polyfills
-------------------------------------------------
if not string.trim then
  function string:trim() return self:match("^%s*(.-)%s*$") end
end
