// primeparts/query/query_preset.h
//
// A query preset — the saved/loaded shape shared between the TUI (which parses
// and serializes these as Lua tables in scripts/lua/) and the reader/query
// layer (which validates them against the catalog schema). See the embedded-Lua
// plan and markdown/arch/tui_app_design.md.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace primeparts::query {

struct QueryField {
  std::string name;
  int64_t value = 0;
};

// `kind` is a string (Lua-friendly, extensible) rather than an enum: "by_k" runs
// QueryService::ScanByK; "lookup" runs LookupPrime (+ partitions).
struct QueryPreset {
  std::string id;                     // unique short name
  std::string desc;                   // human description (format string)
  std::string kind;                   // "by_k" | "lookup"
  std::vector<QueryField> fields;     // variable parameters (k, p_lo, p, ...)
  std::vector<std::string> accepts;   // schema field names it consumes (c-dispatch)
  std::string target;                 // field a dispatched value is written into

  int64_t field(const std::string& n, int64_t dflt = 0) const {
    for (const auto& f : fields)
      if (f.name == n) return f.value;
    return dflt;
  }
};

}  // namespace primeparts::query
