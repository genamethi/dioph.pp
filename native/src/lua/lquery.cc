#include "primeparts/lua/lquery.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "primeparts/lua/lppconv.h"
#include "primeparts/query/query_service.h"

namespace {

using primeparts::lua::BoolOr;
using primeparts::lua::Check;
using primeparts::lua::IntOr;
using primeparts::lua::OptInt;
using primeparts::lua::OptInterval;
using primeparts::lua::ReqInt;
using primeparts::lua::ReqStr;
using primeparts::lua::RequireCatalog;
using primeparts::lua::Spec;
using primeparts::lua::StrArray;
using primeparts::lua::StrOr;

constexpr int64_t kDefaultLimit = 1000000;

void Pin(const sol::table& spec, const char* key, primeparts::scan::Interval* range) {
  if (const std::optional<int64_t> v = OptInt(spec, key)) {
    range->lo = *v;
    range->hi = *v;
  }
}

int64_t Bits(int64_t p) {
  return static_cast<int64_t>(
      63 - __builtin_clzll(static_cast<unsigned long long>(p)));
}

int64_t RangeEnd(const sol::table& spec) {
  const int64_t hi = IntOr(spec, "end", 0);
  return hi != 0 ? hi : IntOr(spec, "hi", 0);
}

sol::table Pget(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "query.pget";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const int64_t p = ReqInt(spec, "p", kFn);
  primeparts::query::QueryService& qs = RequireCatalog(kFn);

  std::string error;
  const std::optional<primeparts::query::PrimeInfo> info = qs.LookupPrime(p, &error);
  Check(error, kFn);
  sol::table out = lua.create_table();
  if (!info) return out;

  primeparts::query::PartitionQuery constraints;
  Pin(spec, "q_k", &constraints.q);
  Pin(spec, "m_k", &constraints.m);
  Pin(spec, "n_k", &constraints.n);
  const std::vector<primeparts::query::PartitionTuple> parts =
      qs.LookupPartitions(p, info->k, constraints, &error);
  Check(error, kFn);

  out["p"] = info->p;
  out["k"] = info->k;
  out["prime_rank"] = info->prime_rank;
  sol::table rows = lua.create_table();
  int idx = 1;
  for (const primeparts::query::PartitionTuple& t : parts) {
    sol::table row = lua.create_table(0, 3);
    row["m"] = t.m_k;
    row["n"] = t.n_k;
    row["q"] = t.q_k;
    rows[idx++] = row;
  }
  out["partitions"] = rows;
  return out;
}

void ReadInterval(const sol::table& spec, const char* key,
                  primeparts::scan::Interval* range, const char* fn) {
  if (const std::optional<std::pair<int64_t, int64_t>> iv =
          OptInterval(spec, key, fn)) {
    range->lo = iv->first;
    range->hi = iv->second;
  }
}

sol::table Partition(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "query.partition";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  primeparts::query::QueryService& qs = RequireCatalog(kFn);

  primeparts::query::PartitionQuery q;
  ReadInterval(spec, "p", &q.p, kFn);
  ReadInterval(spec, "q", &q.q, kFn);
  ReadInterval(spec, "m", &q.m, kFn);
  ReadInterval(spec, "n", &q.n, kFn);

  int64_t limit = IntOr(spec, "limit", kDefaultLimit);
  if (limit <= 0) limit = kDefaultLimit;

  std::string error;
  const std::vector<primeparts::query::PartitionRow> rows =
      qs.ScanPartitions(q, limit, &error);
  Check(error, kFn);

  sol::table out = lua.create_table(static_cast<int>(rows.size()), 0);
  int idx = 1;
  for (const primeparts::query::PartitionRow& r : rows) {
    sol::table row = lua.create_table(0, 4);
    row["p"] = r.p;
    row["m"] = r.m_k;
    row["n"] = r.n_k;
    row["q"] = r.q_k;
    out[idx++] = row;
  }
  return out;
}

sol::table Kget(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "query.kget";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const int64_t k = ReqInt(spec, "k", kFn);
  primeparts::query::QueryService& qs = RequireCatalog(kFn);

  int64_t limit = IntOr(spec, "limit", kDefaultLimit);
  if (limit <= 0) limit = kDefaultLimit;

  std::string error;
  const std::vector<primeparts::query::ScanHit> hits = qs.ScanByK(
      static_cast<int32_t>(k), IntOr(spec, "init", 0), RangeEnd(spec), limit,
      &error);
  Check(error, kFn);

  sol::table out = lua.create_table(static_cast<int>(hits.size()), 0);
  int idx = 1;
  for (const primeparts::query::ScanHit& h : hits) {
    sol::table row = lua.create_table(0, 3);
    row["p"] = h.p;
    row["k"] = k;
    row["prime_rank"] = h.prime_rank;
    out[idx++] = row;
  }
  return out;
}

sol::table Hist(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "query.hist";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const std::string col = ReqStr(spec, "col", kFn);
  const std::string table = StrOr(spec, "table", "primes");
  primeparts::query::QueryService& qs = RequireCatalog(kFn);

  primeparts::query::GroupKey key;
  if (col == "bits") {
    key = primeparts::query::GroupKey::Derived(
        {"p"}, [](const int64_t* v) { return Bits(v[0]); });
  } else if (col == "r") {
    key = primeparts::query::GroupKey::Derived(
        {"p", "k"}, [](const int64_t* v) { return Bits(v[0]) - v[1]; });
  } else {
    key = primeparts::query::GroupKey::Column(col);
  }

  std::string error;
  const std::vector<primeparts::query::GroupCountRow> rows = qs.GroupCount(
      table, key, IntOr(spec, "init", 0), RangeEnd(spec),
      static_cast<int>(IntOr(spec, "threads", 0)), &error);
  Check(error, kFn);

  sol::table out = lua.create_table(static_cast<int>(rows.size()), 0);
  int idx = 1;
  for (const primeparts::query::GroupCountRow& r : rows) {
    sol::table row = lua.create_table(0, 2);
    row[col] = r.value;
    row["count"] = r.count;
    out[idx++] = row;
  }
  return out;
}

std::string Materialize(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "query.materialize";
  const sol::table spec = Spec(ts, arg);
  const std::string name = ReqStr(spec, "name", kFn);
  const std::vector<std::string> cols = StrArray(spec, "cols");
  if (cols.empty()) primeparts::lua::Fail(kFn, "requires cols={...}");

  sol::optional<sol::table> rows = spec["rows"];
  if (!rows) primeparts::lua::Fail(kFn, "requires rows={...}");

  const std::size_t nrows = rows->size();
  std::vector<std::vector<int64_t>> columns(cols.size());
  for (std::vector<int64_t>& c : columns) c.reserve(nrows);
  for (std::size_t r = 1; r <= nrows; ++r) {
    sol::optional<sol::table> row = (*rows)[r];
    if (!row) {
      primeparts::lua::Fail(kFn, "row " + std::to_string(r) + " not a table");
    }
    for (std::size_t j = 0; j < cols.size(); ++j) {
      columns[j].push_back((*row)[cols[j]].get_or(int64_t{0}));
    }
  }

  primeparts::query::QueryService& qs = RequireCatalog(kFn);
  std::string metadata;
  std::string error;
  if (!qs.Materialize(name, cols, columns, &metadata, &error)) {
    primeparts::lua::Fail(kFn, error);
  }
  return metadata;
}

sol::table Read(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "query.read";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const std::string table = ReqStr(spec, "table", kFn);
  primeparts::query::QueryService& qs = RequireCatalog(kFn);

  std::string error;
  const primeparts::query::TableRows res =
      qs.ReadTable(table, StrArray(spec, "cols"), IntOr(spec, "limit", 0), &error);
  Check(error, kFn);

  sol::table out = lua.create_table(static_cast<int>(res.rows.size()), 0);
  int idx = 1;
  for (const std::vector<int64_t>& row : res.rows) {
    sol::table entry = lua.create_table(0, static_cast<int>(res.cols.size()));
    for (std::size_t j = 0; j < res.cols.size(); ++j) entry[res.cols[j]] = row[j];
    out[idx++] = entry;
  }
  return out;
}

sol::table Extent(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "query.extent";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const std::string table = StrOr(spec, "table", "primes");
  primeparts::query::QueryService& qs = RequireCatalog(kFn);

  std::string error;
  const primeparts::query::TableExtent x =
      qs.Extent(table, BoolOr(spec, "key_max", true), &error);
  if (!x.ok) primeparts::lua::Fail(kFn, error.empty() ? "extent unavailable" : error);

  sol::table out = lua.create_table();
  out["table"] = x.table;
  out["snapshots"] = x.snapshots;
  if (x.row_count >= 0) out["count"] = x.row_count;
  if (x.data_files >= 0) out["data_files"] = x.data_files;
  if (x.file_bytes >= 0) out["file_bytes"] = x.file_bytes;
  if (x.snapshot_id >= 0) out["snapshot_id"] = x.snapshot_id;
  if (x.sequence >= 0) out["sequence"] = x.sequence;
  if (!x.key_name.empty()) {
    out["key"] = x.key_name;
    if (x.key_max >= 0) {
      out["key_max"] = x.key_max;
      if (x.key_name == "p") out["max_p"] = x.key_max;
    }
  }
  return out;
}

}  // namespace

extern "C" int luaopen_query(lua_State *L) {
  sol::state_view lua(L);
  sol::table query = lua.create_table();
  query.set_function("pget", &Pget);
  query.set_function("kget", &Kget);
  query.set_function("partition", &Partition);
  query.set_function("hist", &Hist);
  query.set_function("materialize", &Materialize);
  query.set_function("read", &Read);
  query.set_function("extent", &Extent);
  query.push();
  return 1;
}
