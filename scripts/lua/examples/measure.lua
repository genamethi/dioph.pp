-- How far p sits from each of its roots.
-- Run: pp scripts/lua/examples/measure.lua [p]

local p = math.tointeger(tonumber(arg and arg[1] or nil) or 251297)

local D = graph.depth.of{p = p}
print(string.format("p = %d   %d roots   %d chains   longest %d",
  p, #D:roots(), D:count(), D:max()))

local roots = D:roots()
table.sort(roots, function(a, b) return a < b end)

print("root      min  max  chains")
for i = 1, math.min(#roots, 12) do
  local r = roots[i]
  local counts = D:at(r)
  local total, last = 0, 0
  for d, c in ipairs(counts) do
    total = total + c
    if c > 0 then last = d - 1 end
  end
  print(string.format("%-9d %-4d %-4d %d", r, D:min(r), last, total))
end

-- The recursion, run by hand from the parents
local acc
for _, e in ipairs(graph.parts.of{p = p}.partitions) do
  local up = graph.depth.of{p = e.q}:shift()
  acc = acc and (acc + up) or up
end
print("sum of shifted parents matches:", acc:count() == D:count())
