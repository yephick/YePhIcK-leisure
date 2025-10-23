-- debug.lua

local function fmt_tbl(t)
  local function val_str(v)
    local tv = type(v)
    if tv == "string" then
      return v
    elseif tv == "table" then
      local n = 0; for _ in pairs(v) do n = n + 1 end
      return string.format("%s (keys=%d)", tostring(v), n)    -- e.g., "table: 0x1234 (keys=5)"
    else
      return tostring(v)
    end
  end

  local lines = {}
  for k, v in pairs(t) do
    lines[#lines+1] = string.format("{<%s> %s = %s}", type(v), tostring(k), val_str(v))
  end

  table.sort(lines) -- stable, human-friendly order
  return table.concat(lines, ", ")
end

local function fmt_arg(a)
  local t = type(a)
  local tstr = "<" .. t .. ">: "
  if t == "string" then
    local s = a:gsub("\n", "\\n")
    if #s > 200 then s = s:sub(1, 197) .. "..." end
    return tstr .. s
  elseif t == "table" then
    local mt = getmetatable(a)
    local name = mt and mt.__name
    return tstr .. fmt_tbl(a)
  else
    return tstr .. tostring(a)
  end
end

-- Store per-callsite stats and first-hit stack
-- TP_CACHE[site] = { count = <int>, stack = <string>, args = <string> }
local TP_CACHE = {}

-- test point
function TP(...)
  if GetSetting("TP_en", false) ~= true then return end
  local level = 1 + 1               -- 1 = TP itself; +1 = its caller
  local s = debugstack(level, 1, 0) -- example stack line: Interface\AddOns\ATT-GoGo\util.lua:91: in function ...
  local file, line = s:match("([^\n]+):(%d+):")
  local msg = ("%s:%d"):format((file or "?"), (tonumber(line) or -1))
  if TP_CACHE[msg] then
    TP_CACHE[msg].count = TP_CACHE[msg].count + 1
  else
    local n = select('#', ...)
    local args = {...}
    args.n = n -- preserve exact arg count (including trailing nils)
    TP_CACHE[msg] = {
      count = 1,
      stack = debugstack(level, 12, 0),
      args = args,
    }

    for i = 1, n do
      local v = select(i, ...)
      if type(v) == "table" then
        DebugRecursive(v, msg .. " - " .. tostring(v) .. " " .. tostring(i) .. ")", 0, 2, false)
      end
    end

    DebugLog(msg, "trace")
    print("|cff00ff00[ATT-GoGo]|r " .. msg)

    if n > 0 then
      print("args:")
      for ai = 1, n do print(tostring(ai) .. ") " .. fmt_arg(args[ai])) end
    end
  end
end

---- log data types for fields
-- "achID", "achievementID", "awp", "collected", "coords", "creatureID", "eventID", "expansionID", "explorationID", "file", "flightpathID", "icon", "instanceID", "itemID",
-- "link", "mapID", "name", "nmc", "nmr", "npcID", "q", "questID", "r", "rwp", "spellID", "text", "titleID", "title", "visualID", "u"
local ldt_cache = {}
function ldt_nv(name, val)
    local t = type(val)
    local hkey = "<" .. t .. ">" .. name
    if ldt_cache[hkey] then ldt_cache[hkey].count = ldt_cache[hkey].count + 1; return end
    ldt_cache[hkey] = { v = tostring(val), count = 0 }
end

function ldt_nk(node, key) ldt_nv(key, node[key]) end

local function log_ldt()
  for k, v in pairs(ldt_cache) do
    DebugLogf(v.count .. ": [".. k .. "], sample value: " .. v.v)
  end
end

local function TP_summary()
  log_ldt()
  local entries = {}
  for k, v in pairs(TP_CACHE) do
    entries[#entries + 1] = { k, v } -- { site, { count=..., stack=... } }
  end

  if #entries < 1 then return end

  table.sort(entries, function(a, b)
    if a[2].count ~= b[2].count then
      return a[2].count > b[2].count -- sort by count desc
    else
      return a[1] < b[1] -- tie-break by key asc
    end
  end)

  DebugLog("Test Points summary:", "trace")
  for i = 1, #entries do
    local k, v = entries[i][1], entries[i][2]
    DebugLog(("%7d  %s"):format(v.count, k), "trace")
  end

  DebugLog("Test Points first-hit stacks:", "trace")
  for i = 1, #entries do
    local k, v = entries[i][1], entries[i][2]
    DebugLog(("-------- %7d  %s --------"):format(v.count, k), "trace")
    local n = v.args.n
    if n > 0 then
      local parts = {}
      for ai = 1, n do
        parts[ai] = tostring(ai) .. ") " .. fmt_arg(v.args[ai])
      end
      DebugLog("args[" .. n .. "]: " .. table.concat(parts, ", "), "trace")
    end
    local s = v.stack or "(no stack captured)"
    for line in s:gmatch("(.-)\n") do DebugLog(line, "trace") end
  end
end

local plof = CreateFrame("Frame")
plof:RegisterEvent("PLAYER_LOGOUT")
plof:SetScript("OnEvent", TP_summary)


-- Ensure debug table
local function ensure(key)
  ATTGoGoDB       = ATTGoGoDB or {}
  local dbg       = ATTGoGoDB.debug or {}
  ATTGoGoDB.debug = dbg

  key = (type(key) == "string" and key ~= "" and key) or "log"
  dbg[key] = dbg[key] or {}
  return dbg[key]
end

local function push(line, key)
  if line == nil then return end
  local d = ensure(key)
  local s = date("%H:%M:%S ") .. tostring(line)
  if d[#d] == s then return end               -- drop consecutive dupes
  d[#d+1] = s
end

function DebugLog(msg, key) push(msg, key) end
function DebugTrace(msg)    push(msg, "trace") end

-- strip WoW color codes and links; collapse noisy item links
local function sanitize(label)
  if not label then return "?" end
  label = label:gsub("|c%x%x%x%x%x%x%x%x", ""):gsub("|r", "")
  label = label:gsub("|H[^|]-|h%[", "["):gsub("|h", "")
  label = label:gsub("^%s+", ""):gsub("%s+$", "")
  return label
end

-- return true if it's a leaf item/spell/achv link -> skip
local function is_link_spam(label)
  return label:find("|Hitem:", 1, true)
      or label:find("|Hspell:", 1, true)
      or label:find("|Hachievement:", 1, true)
end

-- squash junk labels that flood summaries
local DIFF_NOISE = {
  ["normal"] = true, ["normal+"] = true, ["heroic"] = true, ["heroic+"] = true,
  ["looking for raid"] = true, ["looking for raid+"] = true, ["raid finder"] = true,
  ["flexible"] = true, ["flexible+"] = true,
}
local CLASS_NOISE = {
  ["death knight"]=true, ["druid"]=true, ["hunter"]=true, ["mage"]=true, ["monk"]=true,
  ["paladin"]=true, ["priest"]=true, ["rogue"]=true, ["shaman"]=true, ["warlock"]=true, ["warrior"]=true,
}
local MISC_NOISE = {
  ["?"]=true, ["retrieving data"]=true, ["raid vendor"]=true, ["heroic vendor"]=true,
  ["vendors"]=true, ["heirlooms"]=true, ["quests"]=true, ["achievements"]=true,
  ["zone drop"]=true, ["common boss drop"]=true, ["warforged"]=true, ["thunderforged"]=true,
  ["faction"]=true,
}

-- --- Noise trimming for recursive dumps ---
local SKIP_FIELDS = {
  -- long/unhelpful prose
  lore = true, description = true,

  -- bulky, often with extra codes for colors and links
  text = true,
}

local ONE_LINE_FIELDS = {
  -- show only a one-line summary instead of recursing
  maps = true, providers = true, cost = true, crs = true, coords = true, g = false, parent = false,
}

local MAX_STRLEN = 160  -- clamp very long strings if any slip through
local function clamp_string(s)
  s = tostring(s or "")
  if #s > MAX_STRLEN then return s:sub(1, MAX_STRLEN) .. " …" end
  return s
end

local function print_one_line_table(k, t, depth)
  local n = 0
  for _ in pairs(t) do n = n + 1 end
  DebugLog(string.rep("  ", depth+1) .. "["..tostring(k).."] = {...} (n="..n..")")
end

local function is_noise_label(key)
  if not key or key == "" then return true end
  local k = key:lower()

  -- strip wrapper like "ATTGetProgress( ... )" if passed by mistake
  k = k:gsub("^.-%(", ""):gsub("%)$", "")

  if MISC_NOISE[k] or DIFF_NOISE[k] or CLASS_NOISE[k] then return true end

  -- generic filters
  if k == "unknown" or k == "nil" then return true end
  if k:match("^%[.-%]$") then return true end        -- a bare "[Something]" residual
  if k:match("^%s*$") then return true end
  return false
end

-- ---------- Single entry point for formatted logs ----------
function DebugLogf(fmt, ...)
    push(string.format(fmt, ...))
end

-- Helper: recursive dump, depth-limited
function DebugRecursive(tbl, tblname, depth, maxDepth, showFuncs)
    maxDepth = maxDepth or 2
    depth = depth or 0
    showFuncs = showFuncs == true
    if type(tbl) ~= "table" then
        DebugLog(string.rep("  ", depth) .. tblname .. " = " .. clamp_string(tbl))
        return
    end
    if depth > maxDepth then
        local n = 0
        for _ in pairs(tbl) do n = n + 1 end
        DebugLog(string.rep("  ", depth) .. tblname .. " = {.} (max depth reached; has " .. n .. " children)")
        return
    end
    DebugLog(string.rep("  ", depth) .. tblname .. " {")

    for k, v in pairs(tbl) do
        -- skip noisy keys entirely
        if not SKIP_FIELDS[k] then
            if type(v) == "function" then
                if showFuncs then
                    DebugLog(string.rep("  ", depth+1) .. "["..tostring(k).."] = <function>")
                end
            elseif type(v) == "table" then
                if ONE_LINE_FIELDS[k] then
                    print_one_line_table(k, v, depth)
                elseif tostring(k) == "parent" then
                    DebugLog(string.rep("  ", depth+1) .. "[parent] = {...} (skipped)")
                else
                    DebugRecursive(v, "[" .. tostring(k) .. "]", depth+1, maxDepth, showFuncs)
                end
            else
                DebugLog(string.rep("  ", depth+1) .. "[" .. tostring(k) .. "] = " .. clamp_string(v))
        end
    end
end

    DebugLog(string.rep("  ", depth) .. "}")
end

-- ---------- Debug: print ATT path from root to a node ----------
-- Walks node.parent ... up to root and logs a readable breadcrumb.
-- Usage:
--   DebugPrintNodePath(node)                  -- compact labels
--   DebugPrintNodePath(node, { verbose=true })-- include id tags
--   DebugPrintNodePath(entry.attNode)         -- works with ATT nodes directly
local function firstNonNil(...)
  for i = 1, select('#', ...) do
    local v = select(i, ...)
    if v ~= nil then return v end
  end
end

local function tagList(n)
  local tags = {}
  if n.instanceID     then tags[#tags+1] = "inst:" .. n.instanceID end
  if n.mapID          then tags[#tags+1] = "map:"  .. n.mapID end
  if n.achievementID  then tags[#tags+1] = "ach:"  .. n.achievementID end
  if n.itemID         then tags[#tags+1] = "item:" .. n.itemID end
  if n.spellID        then tags[#tags+1] = "spell:".. n.spellID end
  if n.questID        then tags[#tags+1] = "quest:".. n.questID end
  if n.npcID          then tags[#tags+1] = "npc:"  .. n.npcID end
  if n.expansionID    then tags[#tags+1] = "exp:"  .. n.expansionID end
  return (#tags > 0) and (" [" .. table.concat(tags, ",") .. "]") or ""
end

local function labelFor(n, verbose)
  -- Prefer human-readable label, fall back to typed id
  local lbl = firstNonNil(n.text, n.name)
  if not lbl then
    if n.instanceID    then lbl = "Instance " .. n.instanceID
    elseif n.mapID     then lbl = "Map " .. n.mapID
    elseif n.achievementID then lbl = "Achievement " .. n.achievementID
    elseif n.itemID    then lbl = "Item " .. n.itemID
    elseif n.spellID   then lbl = "Spell " .. n.spellID
    elseif n.questID   then lbl = "Quest " .. n.questID
    elseif n.expansionID then lbl = "Expansion " .. n.expansionID
    else lbl = "<unnamed>"
    end
  end
  -- Reuse the local sanitizer from this file to strip WoW color/link codes
  if type(lbl) == "string" then lbl = sanitize(lbl) end
  if verbose then lbl = lbl .. tagList(n) end
  return lbl
end

--- Print a breadcrumb path from root to the given ATT node.
--- @param node table  -- ATT node (has .parent links)
--- @param opts table? -- { sep=" > ", verbose=true }
function DebugPrintNodePath(node, opts)
  if type(node) ~= "table" then
    DebugLogf("[Path] not a table: %s", tostring(node))
    return
  end
  opts = opts or {}
  local sep     = opts.sep or " > "
  local verbose = opts.verbose == true

  -- Collect labels from node up to root
  local chain, cur, safety = {}, node, 0
  while type(cur) == "table" and safety < 128 do
    chain[#chain+1] = labelFor(cur, verbose)
    cur = rawget(cur, "parent") -- do not trigger metatables
    safety = safety + 1
  end
  -- Reverse to get root -> leaf
  local rev = {}
  for i = #chain, 1, -1 do rev[#rev+1] = chain[i] end

  local path = table.concat(rev, sep)
  DebugLogf("[Path] %s", path)
  return path
end

-- this is a placeholder function to be used as needed, do not remove
local function DebugDump()
    --DebugRecursive(ATT:GetDataCache(), "ATT Root", 0, 2, false)
end

-- Call this on ADDON_LOADED or manually to start a new debug session
function Debug_Init()
  wipe(ensure("log"))
  wipe(ensure("trace"))
  DebugDump()
end
