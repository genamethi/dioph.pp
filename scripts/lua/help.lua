-- help: signatures for the pp-bound modules.
--
--   help              module names
--   help.irc          every entry in irc      help(irc)
--   help.irc.open     one entry               help(irc.open)
--
-- Notation:
--   f{...}            one table argument      f(...)  positional
--   name: type        required                name: type = v  default
--   [T]               array of T              {a: T, ...}  keyed table
--   a | b             alternatives            -> a, b  multiple returns
--   !                 raises

local M = {}

M.nt = {
  order = {"pi", "nth_prime", "next_prime", "prev_prime", "is_prime",
           "is_prime_power", "genparts", "invgp"},
  pi = "nt.pi(n: int) -> int",
  nth_prime = "nt.nth_prime(n: int) -> int",
  next_prime = "nt.next_prime(n: int) -> int",
  prev_prime = "nt.prev_prime(n: int) -> int",
  is_prime = "nt.is_prime(n: int) -> bool",
  is_prime_power = "nt.is_prime_power(n: int) -> base: int, exp: int | nil",
  genparts = [[
nt.genparts(p: int)
  -> {p: int, k: int, partitions: [{p: int, m: int, n: int, q: int}]}
  ! pp_process_prime_array]],
  invgp = [[
nt.invgp(q: int, hi: int = 2^63-1)
  -> {q: int, k: int, partitions: [{p: int, m: int, n: int, q: int}]}
  ! q >= 3]],
}

M.query = {
  order = {"pget", "kget", "partition", "hist", "materialize", "read",
           "extent"},
  pget = [[
query.pget{p: int, q_k: int?, m_k: int?, n_k: int?}
  -> {} | {p: int, k: int, prime_rank: int,
           partitions: [{m: int, n: int, q: int}]}
  ! irc.open]],
  kget = [[
query.kget{k: int, init: int = 0, end: int = 0, hi: int = 0,
           limit: int = 1000000}
  -> [{p: int, k: int, prime_rank: int}]
  ! irc.open]],
  partition = [[
query.partition{p: {int, int} = {3, 2^63-1}, q: {int, int} = {3, 2^63-1},
                m: {int, int} = {1, 2^63-1}, n: {int, int} = {1, 2^63-1},
                limit: int = 1000000}
  -> [{p: int, m: int, n: int, q: int}]
  ! irc.open]],
  hist = [[
query.hist{col: "bits" | "r" | string, table: string = "primes",
           init: int = 0, end: int = 0, hi: int = 0, threads: int = 0}
  -> [{[col]: int, count: int}]
  ! irc.open]],
  materialize = [[
query.materialize{name: string, cols: [string], rows: [{[col]: int}]}
  -> metadata: string
  ! irc.open]],
  read = [[
query.read{table: string, cols: [string] = {}, limit: int = 0}
  -> [{[col]: int}]
  ! irc.open]],
  extent = [[
query.extent{table: string = "primes", key_max: bool = true}
  -> {table: string, snapshots: int, count: int?, data_files: int?,
      file_bytes: int?, snapshot_id: int?, sequence: int?, key: string?,
      key_max: int?, max_p: int?}
  ! irc.open]],
}

M.irc = {
  order = {"open", "info", "close", "reachable", "warehouse",
           "namespace_path", "tables", "bound", "upper_bound",
           "metadata_path", "ensure_namespace", "ensure_table",
           "move_staged"},
  open = [[
irc.open{uri: string = conf.core.rest_uri,
         warehouse: string = conf.core.warehouse,
         namespace: string = conf.core.namespace}
  -> {warehouse: string, uri: string, namespace: string,
      mode: "local" | "rest", attached: bool}]],
  info = [[
irc.info()
  -> {warehouse: string, uri: string, namespace: string,
      mode: "local" | "rest", attached: bool}]],
  close = "irc.close() -> ()",
  reachable = "irc.reachable(uri: string = <current>) -> bool",
  warehouse = "irc.warehouse(uri: string = <current>) -> string",
  namespace_path = "irc.namespace_path(name: string = <current>) -> string",
  tables = "irc.tables() -> [string]\n  ! irc.open",
  bound = [[
irc.bound{table: string, field: string}
  -> int | nil,
     "kTableAbsent" | "kNoSnapshot" | "kSnapshotNoBound" | "kPresent"
  ! irc.open]],
  upper_bound = [[
irc.upper_bound{table: string, field: string}
  -> int | nil, present: bool
  ! irc.open]],
  metadata_path = "irc.metadata_path(spec: table?) -> !unimplemented",
  ensure_namespace = "irc.ensure_namespace(spec: table?) -> !unimplemented",
  ensure_table = "irc.ensure_table(spec: table?) -> !unimplemented",
  move_staged = "irc.move_staged(spec: table?) -> !unimplemented",
}

M.graph = {
  order = {"parts", "depth", "words", "walk", "twists", "expr", "poly"},
  parts = "graph.parts -> module   of, calls, source",
  depth = "graph.depth -> module   of, root, Depth",
  words = "graph.words -> module   of, root, Words",
  walk = "graph.walk -> module   reach, chains, forms, skeleton",
  twists = "graph.twists -> module   build, TwistTable",
  expr = "graph.expr -> module   sym, int, pow2, Expr",
  poly = "graph.poly -> module   he, x, const, pow2, Poly",
}

M["graph.parts"] = {
  order = {"of", "calls", "source"},
  of = [[
graph.parts.of{p: int} | graph.parts{p: int}
  -> {p: int, k: int, partitions: [{p: int, m: int, n: int, q: int}]}]],
  calls = "graph.parts.calls() -> int",
  source = "graph.parts.source() -> string",
}

M["graph.depth"] = {
  order = {"of", "root"},
  of = [[
graph.depth.of{p: int, depth: int = -1, max_cells: int = 64000000} -> Depth
graph.depth{p: int, ...} -> Depth
  ! max_cells exceeded]],
  root = "graph.depth.root(r: int) -> Depth",
}

M["graph.words"] = {
  order = {"of", "root"},
  of = [[
graph.words.of{p: int, depth: int = -1, max_cells: int = 64000000,
               targets: [int] | [{p: int}] = nil} -> Words
graph.words{p: int, ...} -> Words
  ! max_cells exceeded]],
  root = "graph.words.root(r: int) -> Words",
}

M["graph.walk"] = {
  order = {"reach", "chains", "forms", "skeleton"},
  reach = [[
graph.walk.reach{p: int, depth: int = 8, max_nodes: int = 1000000,
                 twists: TwistTable?}
  -> {levels: [{depth: int, count: int, nodes: [int]}],
      twists: [{p: int, depth: int}],
      nodes: int, calls: int}
  ! max_nodes exceeded]],
  chains = [[
graph.walk.chains{p: int, depth: int = 8, sources: bool = false,
                  targets: [int] | [{p: int}] = nil}
  -> [{terminal: int, length: int, twists: int, degree: int,
       edges: [{p, m, n, q}]}], .meta = {calls: int}]],
  forms = [[
graph.walk.forms{p: int, depth: int = 8, sources: bool = false,
                 targets: [int] | [{p: int}] = nil,
                 symbol: string = "x", distinct: bool = true}
  -> [{terminal: int, length: int, twists: int, degree: int,
       edges: [{p, m, n, q}], paths: int, expr: Expr, exact: bool,
       word: {a0: int, blocks: [{n: int, c: int}], skeleton: [int],
              degree: int, grade: int, odd_degree: int, terminal: int}}]
     .meta = {calls: int, symbol: string}]],
  skeleton = [[
graph.walk.skeleton{skeleton: [int], hi: int, roots: [int] | [{p: int}] = nil,
                    symbol: string = "x", base: string = ""}
  -> [{p: int, terminal: int, degree: int, expr: Expr, exact: bool,
       word: {a0: int, blocks: [{n: int, c: int}], skeleton: [int],
              degree: int, grade: int, odd_degree: int, terminal: int}}]
     .meta = {roots: [int], bounds: [int], nodes: int, symbol: string}
  ! every skeleton entry >= 2, hi >= 3]],
}

M["graph.twists"] = {
  order = {"build"},
  build = [[
graph.twists.build{hi: int} -> TwistTable
graph.twists.build{rows: [{p, m, n, q}], hi: int = max p} -> TwistTable
graph.twists{...} -> TwistTable]],
}

M["graph.expr"] = {
  order = {"sym", "int", "pow2", "lift"},
  sym = "graph.expr.sym(name: string) -> Expr",
  int = "graph.expr.int(v: int) -> Expr",
  pow2 = "graph.expr.pow2(m: int) -> Expr",
  lift = "graph.expr.lift(v: int, t: string) -> Expr   v in base t",
}

M["graph.poly"] = {
  order = {"he", "lift", "x", "const", "pow2"},
  he = "graph.poly.he(n: int) -> Poly",
  lift = "graph.poly.lift(v: int) -> Poly   v in base t, binary expansion",
  x = "graph.poly.x() -> Poly",
  const = "graph.poly.const(c: int) -> Poly",
  pow2 = "graph.poly.pow2(m: int) -> Poly",
}

M.Expr = [[
Expr   symbolic, held unexpanded (GiNaC)
  + - * ^ between Exprs; tostring
  :text() :expand() :numeric() :symbols() :subs(sym, int | Expr) :value()
  :degree(sym) :ldegree(sym) :coeff(sym, k) -> Expr
  :head() -> "add" | "mul" | "power" | "symbol" | "numeric" | "function"
  :arity() :op(i) -> Expr    the GiNaC tree, one node at a time
  :poly(sym) -> Poly     :he(sym) -> [{i, c}] Hermite coefficients]]

M.Poly = [[
Poly   exact integer univariate (FLINT)
  + - * and == between Polys; tostring
  :degree() :zero() :text(var) :mono() :he() :eval(x) :pow(e) :mod(n)]]

M.Depth = [[
Depth   chains into p by length, per root
  D(p) = z * sum over q in K(p) of D(q);  D(r) = 1 at a root
  + between Depths; :shift() multiplies by z
  :roots() :at(r) -> [int] counts by length  :min(r) :max() :count()
  :cut(d) :known() -> int, -1 when complete  :complete() :cells()]]

M.Words = [[
Words   the words of p, graded by chain length
  W(r) = the empty word at a root
  W(p) = sum over (m,n,q) in K(p) of W(q):step(m, n)
  + between Words; :step(m, n) applies one edge
  :rows(symbol, base) -> [{word, terminal, degree, expr, paths, by_depth}]
    base names a symbol for 2, so constants come back as sums of base^m
  :size() :count() :cut(d) :known() :complete() :cells()
  :grade() -> Depth   forget the word, keep the lengths]]

M.TwistTable = [[
TwistTable   the n >= 2 edges below hi
  :hi() :origin() :count() :has(p) :at(p) :into(q) :rows(limit)]]

local modules = {"nt", "query", "irc", "graph"}
local submodules = {"graph.parts", "graph.depth", "graph.words",
                    "graph.walk", "graph.twists", "graph.expr", "graph.poly"}

-- A node renders one entry or a whole module; help.x.y and help(x.y) both
-- resolve to the same node.

local entry_mt = {
  __tostring = function(self) return self.text end,
  __name = "help",
}

local function entry(text) return setmetatable({text = text}, entry_mt) end

local nodes = {}

local module_mt = {
  __index = function(self, key)
    local name = rawget(self, "name")
    local child = nodes[name .. "." .. key]
    if child then return child end
    local text = M[name][key]
    if type(text) == "string" then return entry(text) end
    return nil
  end,
  __tostring = function(self)
    local spec = M[rawget(self, "name")]
    local out = {}
    for _, key in ipairs(spec.order) do out[#out + 1] = spec[key] end
    return table.concat(out, "\n")
  end,
  __name = "help",
}

local function resolve(name)
  local mod = _G
  for part in name:gmatch("[^.]+") do
    if type(mod) ~= "table" then return nil end
    mod = mod[part]
  end
  return mod
end

local help = {}
local byvalue = {}

for _, name in ipairs(modules) do
  nodes[name] = setmetatable({name = name}, module_mt)
  help[name] = nodes[name]
end
for _, name in ipairs(submodules) do
  nodes[name] = setmetatable({name = name}, module_mt)
end

for name, node in pairs(nodes) do
  local mod = resolve(name)
  if type(mod) == "table" then
    byvalue[mod] = node
    for key, text in pairs(M[name]) do
      if key ~= "order" and type(mod[key]) == "function" then
        byvalue[mod[key]] = entry(text)
      end
    end
  end
end

return setmetatable({}, {
  __index = help,
  __call = function(_, subject)
    if subject == nil then return entry(table.concat(modules, "\n")) end
    return byvalue[subject]
  end,
  __tostring = function() return table.concat(modules, "\n") end,
  __name = "help",
})
