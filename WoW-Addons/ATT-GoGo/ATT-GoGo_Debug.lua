-- ATT-GoGo_Debug.lua

-- Ensure debug table
ATTGoGoDebugDB = {}

local function ensure()
  ATTGoGoDB = ATTGoGoDB or {}
  ATTGoGoDB.debug = ATTGoGoDB.debug or { log = {} }
  return ATTGoGoDB.debug
end

local function push(line)
  local d = ensure()
  local L = d.log
  local s = date("%H:%M:%S ") .. tostring(line)
  if L[#L] == s then return end               -- drop consecutive dupes
  L[#L+1] = s
end


function DebugLog(msg)
    if msg then
        push(msg)
    end
end

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

  -- perf/visual/no-coding
  visible = true, nmr = true, nmc = true,
  --awp = true,

  -- flags rarely needed for structure
  u = true, lvl = true, text = true,

  -- bulky coords we don't need line-by-line
  coords = true,
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
        DebugLog(string.rep("  ", depth) .. tblname .. " = {...} (max depth reached)")
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
  if n.instanceID     then tags[#tags+1] = "inst:" .. tostring(n.instanceID) end
  if n.mapID          then tags[#tags+1] = "map:"  .. tostring(n.mapID) end
  if n.achievementID  then tags[#tags+1] = "ach:"  .. tostring(n.achievementID) end
  if n.itemID         then tags[#tags+1] = "item:" .. tostring(n.itemID) end
  if n.spellID        then tags[#tags+1] = "spell:".. tostring(n.spellID) end
  if n.questID        then tags[#tags+1] = "quest:".. tostring(n.questID) end
  if n.npcID          then tags[#tags+1] = "npc:"  .. tostring(n.npcID) end
  if n.expansionID    then tags[#tags+1] = "exp:"  .. tostring(n.expansionID) end
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
  local d = ensure()
  wipe(d.log)
  DebugDump()
end
