--touched = true

conf = {
  core = {
    rest_uri  = "http://127.0.0.1:8181",
    namespace = "primeparts",
    warehouse = "~/.local/share/pp-data",
  },

  generate = {
    threads      = 0,
    chunk_primes = 500000,
  },

  catalogd = {
    host               = "127.0.0.1",
    port               = 8181,
    scan_planning_mode = "server",
    plan_batch         = 64,
    plan_ttl           = 300,
  },

  verify = {
    threads      = 0,
    max_examples = 20,
  },

  query = {
    limit = 10,
  },

  graph = {
    threads = 8,
    top     = 20,
    max_p   = 100000,
    mode    = "basis",
    format  = "text",
  },

  tui = {
    log_limit     = 200,
    default_limit = 10,
    log_format    = "flat",
    autosave      = false,
  },
}

return conf
