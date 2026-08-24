-- Build every chain with a given skeleton.
-- Run: pp scripts/lua/examples/build.lua [4,2] [hi]

local spec = (arg and arg[1]) or "4,2"
local hi = math.tointeger(tonumber(arg and arg[2] or nil) or 1e9)

local skel = {}
for n in spec:gmatch("%d+") do skel[#skel + 1] = math.tointeger(tonumber(n)) end

local r = graph.walk.skeleton{skeleton = skel, hi = hi, base = "t"}
print(string.format("[%s] under %d: %d chains, roots %s",
  table.concat(skel, ","), hi, #r, table.concat(r.meta.roots, " ")))
print("bounds per level: " .. table.concat(r.meta.bounds, "  "))

-- Chains sharing a word differ only in the twist m
table.sort(r, function(a, b)
  if a.p ~= b.p then return a.p < b.p end
  return a.edges[1].m < b.edges[1].m
end)
for i = 1, math.min(#r, 10) do
  local via = {}
  for _, e in ipairs(r[i].edges) do
    via[#via + 1] = string.format("%d -(2^%d,^%d)-> %d", e.q, e.m, e.n, e.p)
  end
  print(string.format("  p=%-12d %s", r[i].p, table.concat(via, "  ")))
  print(string.format("               %s", tostring(r[i].expr)))
end
