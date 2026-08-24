-- Pick interesting p from the warehouse, then decompose.
-- Run: pp scripts/lua/examples/catalog.lua [hi]

local hi = math.tointeger(tonumber(arg and arg[1] or nil) or 5e5)

irc.open()
local roots = query.kget{k = 0, limit = 200}

-- Twist edges are the rare ones; they carry all degree
local seen, cand = {}, {}
for _, row in ipairs(query.partition{p = {3, hi}, n = {2, 63}}) do
  if not seen[row.p] then
    seen[row.p] = true
    cand[#cand + 1] = row.p
  end
end
print(string.format("%d twist targets under %d", #cand, hi))

local best = {}
for _, p in ipairs(cand) do
  local W = graph.words.of{p = p, targets = roots}
  local skel = {}
  for _, r in ipairs(W:rows("x")) do
    skel[table.concat(r.word.skeleton, ",")] = true
  end
  local n = 0
  for _ in pairs(skel) do n = n + 1 end
  best[#best + 1] = {p = p, skeletons = n, words = W:size()}
end

table.sort(best, function(a, b) return a.skeletons > b.skeletons end)
print("p          skeletons  words")
for i = 1, math.min(#best, 10) do
  print(string.format("%-10d %-10d %d", best[i].p, best[i].skeletons,
                      best[i].words))
end
