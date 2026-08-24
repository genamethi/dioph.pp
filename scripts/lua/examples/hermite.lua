-- Every word into p, in the Hermite basis.
-- Run: pp scripts/lua/examples/hermite.lua [p]

local p = math.tointeger(tonumber(arg and arg[1] or nil) or 65537)

local function he_text(e)
  local terms = {}
  for _, c in ipairs(e:he("x")) do
    local v = tostring(c.c)
    if v ~= "0" then
      terms[#terms + 1] = (v == "1") and ("He" .. c.i) or (v .. " He" .. c.i)
    end
  end
  -- he() ascends; m2_bench prints descending
  for i = 1, #terms // 2 do
    terms[i], terms[#terms - i + 1] = terms[#terms - i + 1], terms[i]
  end
  return table.concat(terms, " + ")
end

local W = graph.words.of{p = p}
print(string.format("p = %d   %d words over %d chains", p, W:size(), W:count()))

local rows = W:rows("x")
table.sort(rows, function(a, b)
  if a.degree ~= b.degree then return a.degree < b.degree end
  return a.paths > b.paths
end)

for _, r in ipairs(rows) do
  if r.degree > 1 then
    print(string.format("mult %-9d [%s] root=%-5d %s",
      r.paths, table.concat(r.word.skeleton, ","), r.terminal, he_text(r.expr)))
  end
end
