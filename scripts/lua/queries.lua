-- primeparts query presets.
--
-- Loaded by the TUI (it owns the lua_State) and validated by the reader
-- (QueryService::ValidatePreset) against the catalog schema. Edit by hand or
-- save from the TUI. Each entry:
--   query("id", { desc=, kind=, fields={name=value,...}, accepts={...}, target= })
--   kind: "by_k" (QueryService.ScanByK) | "lookup" (LookupPrime + partitions)
--   accepts/target: schema field names (p, k, q_k, m_k, n_k, prime_rank)

query("by-k", {
  desc = "primes where k == {k}, p in [{p_lo},{p_hi}], LIMIT {limit}",
  kind = "by_k",
  fields = { k = 0, p_lo = 0, p_hi = 0, limit = 10 },
  accepts = { "k" },
  target = "k",
})

query("lookup", {
  desc = "prime p == {p}  ->  k + partitions",
  kind = "lookup",
  fields = { p = 11 },
  accepts = { "p", "q_k" },
  target = "p",
})
