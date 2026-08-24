-- Normalized state S = P - p over Z/2^e.
-- Run: pp scripts/lua/examples/reduce.lua [p] [e]

local p = math.tointeger(tonumber(arg and arg[1] or nil) or 65537)
local e = math.tointeger(tonumber(arg and arg[2] or nil) or 5)

local mod = "1"
for _ = 1, e do mod = tostring(math.tointeger(tonumber(mod)) * 2) end
print(string.format("p = %d   level e = %d   modulus %s", p, e, mod))

local rows = graph.words.of{p = p}:rows("x")
table.sort(rows, function(a, b)
  if a.degree ~= b.degree then return a.degree < b.degree end
  return a.paths > b.paths
end)

local seen = {}
for _, r in ipairs(rows) do
  local S = (r.expr - graph.expr.int(p)):poly("x")
  local at_e = tostring(S:mod(mod))
  local at_1 = tostring(S:mod("2"))
  local key = at_e
  if not seen[key] then
    seen[key] = true
    -- E4: mod 2 the state is x^N and nothing else
    print(string.format("N=%-3d [%-7s] mod 2: %-10s mod %s: %s",
      r.degree, table.concat(r.word.skeleton, ","), at_1, mod, at_e))
  end
end

local classes = 0
for _ in pairs(seen) do classes = classes + 1 end
print(string.format("%d distinct states at level %d", classes, e))
