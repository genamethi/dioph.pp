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
           "is_prime_power", "genparts"},
  pi = "nt.pi(n: int) -> int",
  nth_prime = "nt.nth_prime(n: int) -> int",
  next_prime = "nt.next_prime(n: int) -> int",
  prev_prime = "nt.prev_prime(n: int) -> int",
  is_prime = "nt.is_prime(n: int) -> bool",
  is_prime_power = "nt.is_prime_power(n: int) -> base: int, exp: int | nil",
  genparts = [[
nt.genparts(p: int)
  -> {p: int, k: int, partitions: [{m: int, n: int, q: int}]}
  ! pp_process_prime_array]],
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

local modules = {"nt", "query", "irc"}

-- A node renders one entry or a whole module; help.x.y and help(x.y) both
-- resolve to the same node.

local entry_mt = {
  __tostring = function(self) return self.text end,
  __name = "help",
}

local function entry(text) return setmetatable({text = text}, entry_mt) end

local module_mt = {
  __index = function(self, key)
    local text = M[rawget(self, "name")][key]
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

local help = {}
local byvalue = {}

for _, name in ipairs(modules) do
  local node = setmetatable({name = name}, module_mt)
  help[name] = node
  local mod = _G[name]
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
