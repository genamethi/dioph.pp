#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "duckdb.hpp"

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"

#include <ginac/ginac.h>

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/rest_scan_plan.h"
#include "primeparts/client/session.h"
#include "primeparts/graph/pp_graph_store.h"
#include "primeparts/query/materialize.h"
#include "primeparts/scan/scan_plan.h"

namespace ppc = primeparts::catalog;
namespace client = primeparts::client;
namespace scan = primeparts::scan;

namespace {

constexpr int64_t kDefaultMax = 100000;

struct Options {
  std::string rest_uri;
  std::string warehouse;
  std::string ns_name = ppc::kDefaultNamespace;
  int64_t min_p = 0;
  int64_t max_p = kDefaultMax;
  int64_t threads = 8;
  std::string format = "text";
  std::string mode = "basis";
  int64_t top = 20;
};

struct Edge {
  int64_t q;
  int64_t p;
  int32_t m;
  int32_t n;
};

int64_t IPow(int64_t base, int32_t exp, bool* overflow) {
  int64_t r = 1;
  *overflow = false;
  for (int32_t i = 0; i < exp; ++i) {
    if (base != 0 && r > INT64_MAX / base) {
      *overflow = true;
      return 0;
    }
    r *= base;
  }
  return r;
}

int64_t IRoot(int64_t value, int32_t n) {
  if (value < 0) return -1;
  if (n <= 0) return -1;
  if (n == 1) return value;
  int64_t lo = 0;
  int64_t hi = 1;
  while (true) {
    bool ov = false;
    const int64_t t = IPow(hi, n, &ov);
    if (ov || t >= value) break;
    hi *= 2;
  }
  while (lo <= hi) {
    const int64_t mid = lo + (hi - lo) / 2;
    bool ov = false;
    const int64_t t = IPow(mid, n, &ov);
    if (ov || t > value) {
      hi = mid - 1;
    } else if (t < value) {
      lo = mid + 1;
    } else {
      return mid;
    }
  }
  return -1;
}

bool SolveQ(int64_t p, int32_t m, int32_t n, int64_t* q) {
  bool ov = false;
  const int64_t two_m = IPow(2, m, &ov);
  if (ov || two_m >= p) return false;
  const int64_t rem = p - two_m;
  const int64_t root = IRoot(rem, n);
  if (root < 2) return false;
  *q = root;
  return true;
}

std::string QuoteList(const std::vector<std::string>& paths) {
  std::string s = "[";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i) s += ", ";
    s += "'" + paths[i] + "'";
  }
  s += "]";
  return s;
}

double Seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
      .count();
}

void Usage(FILE* out) {
  std::fprintf(
      out,
      "usage: pp-graph [options]\n"
      "\n"
      "Reads primeparts.partitions over the REST catalog, recovers each edge's\n"
      "q_k algebraically from (p, m_k, n_k), and reports the induced poset on\n"
      "the queried prime window: the Hasse diagram (cover relation), a minimum\n"
      "chain decomposition, and the antichains witnessing its width.\n"
      "\n"
      "Options:\n"
      "  --min P          low end of the prime window (default 0)\n"
      "  --max P          high end of the prime window (default %" PRId64 ")\n"
      "  --rest-uri URL   pp-catalogd base (default %s)\n"
      "  --warehouse DIR  warehouse root\n"
      "  --namespace NS   catalog namespace (default %s)\n"
      "  --threads N      scan/engine threads (default 8)\n"
      "  --mode M         basis | hasse (default basis)\n"
      "  --format F       text | dot | json (default text)\n"
      "  --top N          rows to show in text listings (default 20)\n"
      "  --help\n",
      kDefaultMax, ppc::kDefaultRestUri, ppc::kDefaultNamespace);
}

bool ParseI64(const char* s, int64_t* out) {
  char* end = nullptr;
  errno = 0;
  const long long v = std::strtoll(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool ParseArgs(int argc, char** argv, Options* opt) {
  static const option longopts[] = {
      {"min", required_argument, nullptr, 1000},
      {"max", required_argument, nullptr, 1001},
      {"rest-uri", required_argument, nullptr, 1002},
      {"warehouse", required_argument, nullptr, 1003},
      {"namespace", required_argument, nullptr, 1004},
      {"threads", required_argument, nullptr, 1005},
      {"format", required_argument, nullptr, 1006},
      {"top", required_argument, nullptr, 1007},
      {"mode", required_argument, nullptr, 1008},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  int c;
  while ((c = getopt_long(argc, argv, "h", longopts, nullptr)) != -1) {
    switch (c) {
      case 1000:
        if (!ParseI64(optarg, &opt->min_p)) return false;
        break;
      case 1001:
        if (!ParseI64(optarg, &opt->max_p)) return false;
        break;
      case 1002: opt->rest_uri = optarg; break;
      case 1003: opt->warehouse = optarg; break;
      case 1004: opt->ns_name = optarg; break;
      case 1005:
        if (!ParseI64(optarg, &opt->threads)) return false;
        break;
      case 1006: opt->format = optarg; break;
      case 1007:
        if (!ParseI64(optarg, &opt->top)) return false;
        break;
      case 1008: opt->mode = optarg; break;
      case 'h': Usage(stdout); std::exit(0);
      default: return false;
    }
  }
  if (opt->rest_uri.empty()) opt->rest_uri = ppc::kDefaultRestUri;
  if (opt->max_p <= opt->min_p) {
    std::fprintf(stderr, "--max must exceed --min\n");
    return false;
  }
  return true;
}

struct Poset {
  std::vector<int64_t> node;
  std::unordered_map<int64_t, int32_t> index;
  std::vector<std::vector<int32_t>> up;
  std::vector<std::vector<int32_t>> down;
};

Poset BuildPoset(const std::vector<Edge>& edges) {
  Poset g;
  auto intern = [&](int64_t v) {
    auto it = g.index.find(v);
    if (it != g.index.end()) return it->second;
    const int32_t id = static_cast<int32_t>(g.node.size());
    g.node.push_back(v);
    g.index.emplace(v, id);
    return id;
  };
  std::vector<std::pair<int32_t, int32_t>> pairs;
  pairs.reserve(edges.size());
  for (const auto& e : edges) pairs.emplace_back(intern(e.q), intern(e.p));
  g.up.resize(g.node.size());
  g.down.resize(g.node.size());
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  for (const auto& [a, b] : pairs) {
    g.up[a].push_back(b);
    g.down[b].push_back(a);
  }
  return g;
}

std::vector<int32_t> TopoOrder(const Poset& g) {
  std::vector<int32_t> indeg(g.node.size(), 0);
  for (int32_t v = 0; v < static_cast<int32_t>(g.node.size()); ++v)
    for (int32_t w : g.up[v]) ++indeg[w];
  std::queue<int32_t> q;
  for (int32_t v = 0; v < static_cast<int32_t>(g.node.size()); ++v)
    if (indeg[v] == 0) q.push(v);
  std::vector<int32_t> order;
  order.reserve(g.node.size());
  while (!q.empty()) {
    const int32_t v = q.front();
    q.pop();
    order.push_back(v);
    for (int32_t w : g.up[v])
      if (--indeg[w] == 0) q.push(w);
  }
  return order;
}

std::vector<int32_t> MirskyLevels(const Poset& g,
                                  const std::vector<int32_t>& order) {
  std::vector<int32_t> level(g.node.size(), 0);
  for (int32_t v : order)
    for (int32_t w : g.up[v]) level[w] = std::max(level[w], level[v] + 1);
  return level;
}

struct Closure {
  int32_t n = 0;
  size_t words = 0;
  std::vector<uint64_t> bits;

  bool Get(int32_t v, int32_t w) const {
    return (bits[static_cast<size_t>(v) * words + (w >> 6)] >> (w & 63)) & 1ULL;
  }
  void Set(int32_t v, int32_t w) {
    bits[static_cast<size_t>(v) * words + (w >> 6)] |= 1ULL << (w & 63);
  }
  void Or(int32_t dst, int32_t src) {
    uint64_t* d = &bits[static_cast<size_t>(dst) * words];
    const uint64_t* s = &bits[static_cast<size_t>(src) * words];
    for (size_t i = 0; i < words; ++i) d[i] |= s[i];
  }
};

Closure StrictReachability(const Poset& g, const std::vector<int32_t>& order) {
  Closure c;
  c.n = static_cast<int32_t>(g.node.size());
  c.words = (static_cast<size_t>(c.n) + 63) / 64;
  c.bits.assign(static_cast<size_t>(c.n) * c.words, 0ULL);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const int32_t v = *it;
    for (int32_t w : g.up[v]) {
      c.Set(v, w);
      c.Or(v, w);
    }
  }
  return c;
}

std::vector<std::pair<int32_t, int32_t>> CoverRelation(const Poset& g,
                                                       const Closure& c) {
  std::vector<std::pair<int32_t, int32_t>> cover;
  for (int32_t v = 0; v < static_cast<int32_t>(g.node.size()); ++v) {
    for (int32_t w : g.up[v]) {
      bool detour = false;
      for (int32_t u : g.up[v]) {
        if (u == w) continue;
        if (c.Get(u, w)) {
          detour = true;
          break;
        }
      }
      if (!detour) cover.emplace_back(v, w);
    }
  }
  return cover;
}

class Matching {
 public:
  explicit Matching(int32_t n) : n_(n), adj_(n), match_l_(n, -1), match_r_(n, -1) {}

  void Add(int32_t l, int32_t r) { adj_[l].push_back(r); }

  int32_t Solve() {
    int32_t size = 0;
    for (int32_t l = 0; l < n_; ++l) {
      std::vector<char> seen(n_, 0);
      if (Augment(l, &seen)) ++size;
    }
    return size;
  }

  const std::vector<int32_t>& match_l() const { return match_l_; }

 private:
  bool Augment(int32_t l, std::vector<char>* seen) {
    for (int32_t r : adj_[l]) {
      if ((*seen)[r]) continue;
      (*seen)[r] = 1;
      if (match_r_[r] == -1 || Augment(match_r_[r], seen)) {
        match_r_[r] = l;
        match_l_[l] = r;
        return true;
      }
    }
    return false;
  }

  int32_t n_;
  std::vector<std::vector<int32_t>> adj_;
  std::vector<int32_t> match_l_;
  std::vector<int32_t> match_r_;
};

std::vector<std::vector<int32_t>> ChainDecomposition(const Poset& g,
                                                     const Closure& c,
                                                     int32_t* matched) {
  const int32_t n = static_cast<int32_t>(g.node.size());
  Matching m(n);
  for (int32_t v = 0; v < n; ++v)
    for (int32_t w = 0; w < n; ++w)
      if (c.Get(v, w)) m.Add(v, w);
  *matched = m.Solve();
  const auto& next = m.match_l();
  std::vector<char> has_pred(n, 0);
  for (int32_t v = 0; v < n; ++v)
    if (next[v] >= 0) has_pred[next[v]] = 1;
  std::vector<std::vector<int32_t>> chains;
  for (int32_t v = 0; v < n; ++v) {
    if (has_pred[v]) continue;
    std::vector<int32_t> chain;
    for (int32_t u = v; u >= 0; u = next[u]) chain.push_back(u);
    chains.push_back(std::move(chain));
  }
  return chains;
}

struct Generator {
  int32_t m = 0;
  int32_t n = 0;
  int64_t occurrences = 0;
  std::map<int, GiNaC::ex> hermite;
  bool pivot = false;
};

bool PlanPaths(const Options& opt, const std::string& table,
               const scan::ScanPlanRequest& request,
               std::vector<std::string>* paths, double* t_plan,
               std::string* error) {
  client::SessionOptions so;
  so.rest_uri = opt.rest_uri;
  so.warehouse = opt.warehouse;
  so.ns = opt.ns_name;
  so.scan_threads = static_cast<int>(opt.threads);
  auto session = client::Session::Open(so, error);
  if (!session) return false;
  client::TableHandle handle;
  if (!session->LoadTable(table, &handle, error)) return false;
  const auto ns = ppc::ResolveNamespace(opt.ns_name);
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  if (!ppc::PlanScanOnServer(opt.rest_uri, ns, table, request,
                             *handle.metadata(), ppc::PlanPollOptions{}, &tasks,
                             error)) {
    return false;
  }
  *t_plan = Seconds(t0);
  for (const auto& t : tasks) paths->push_back(t->data_file()->file_path);
  return true;
}

bool ReduceToBasis(std::vector<Generator>* gens, std::vector<int>* degrees) {
  std::set<int> deg_set;
  for (const auto& g : *gens)
    for (const auto& [d, c] : g.hermite) deg_set.insert(d);
  degrees->assign(deg_set.begin(), deg_set.end());
  const size_t width = degrees->size();

  std::vector<std::vector<GiNaC::ex>> rows;
  rows.reserve(gens->size());
  for (const auto& g : *gens) {
    std::vector<GiNaC::ex> r(width, GiNaC::ex(0));
    for (size_t j = 0; j < width; ++j) {
      auto it = g.hermite.find((*degrees)[j]);
      if (it != g.hermite.end()) r[j] = it->second;
    }
    rows.push_back(std::move(r));
  }

  std::vector<std::vector<GiNaC::ex>> reduced;
  std::vector<size_t> pivot_col;
  for (size_t i = 0; i < rows.size(); ++i) {
    std::vector<GiNaC::ex> r = rows[i];
    for (size_t k = 0; k < reduced.size(); ++k) {
      const GiNaC::ex factor = GiNaC::normal(r[pivot_col[k]]);
      if (factor.is_zero()) continue;
      for (size_t j = 0; j < width; ++j)
        r[j] = GiNaC::normal(r[j] - factor * reduced[k][j]);
    }
    size_t lead = width;
    for (size_t j = 0; j < width; ++j) {
      if (!GiNaC::normal(r[j]).is_zero()) {
        lead = j;
        break;
      }
    }
    if (lead == width) continue;
    const GiNaC::ex inv = GiNaC::normal(1 / r[lead]);
    for (size_t j = 0; j < width; ++j) r[j] = GiNaC::normal(r[j] * inv);
    reduced.push_back(std::move(r));
    pivot_col.push_back(lead);
    (*gens)[i].pivot = true;
  }
  return true;
}

bool ReadEdges(const Options& opt, std::vector<Edge>* out, int64_t* rows_scanned,
               double* t_plan, double* t_read, std::string* error) {
  client::SessionOptions so;
  so.rest_uri = opt.rest_uri;
  so.warehouse = opt.warehouse;
  so.ns = opt.ns_name;
  so.scan_threads = static_cast<int>(opt.threads);
  auto session = client::Session::Open(so, error);
  if (!session) return false;

  client::TableHandle partitions;
  if (!session->LoadTable("partitions", &partitions, error)) return false;

  scan::ScanPlanRequest request;
  request.select = {"p", "m_k", "n_k"};
  request.filter = iceberg::Expressions::And(
      iceberg::Expressions::GreaterThanOrEqual(
          "p", iceberg::Literal::Long(opt.min_p)),
      iceberg::Expressions::LessThanOrEqual("p",
                                            iceberg::Literal::Long(opt.max_p)));

  const auto ns = ppc::ResolveNamespace(opt.ns_name);
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  if (!ppc::PlanScanOnServer(opt.rest_uri, ns, "partitions", request,
                             *partitions.metadata(), ppc::PlanPollOptions{},
                             &tasks, error)) {
    return false;
  }
  *t_plan = Seconds(t0);

  std::vector<std::string> paths;
  paths.reserve(tasks.size());
  for (const auto& t : tasks) paths.push_back(t->data_file()->file_path);
  if (paths.empty()) return true;

  duckdb::DBConfig cfg;
  cfg.SetOptionByName("threads", duckdb::Value::BIGINT(opt.threads));
  duckdb::DuckDB db(nullptr, &cfg);
  duckdb::Connection con(db);

  const std::string sql =
      "SELECT p, m_k, n_k FROM read_parquet(" + QuoteList(paths) +
      ") WHERE p >= " + std::to_string(opt.min_p) +
      " AND p <= " + std::to_string(opt.max_p);

  t0 = std::chrono::steady_clock::now();
  auto result = con.Query(sql);
  if (result->HasError()) {
    *error = result->GetError();
    return false;
  }
  for (auto& chunk : *result) {
    const int64_t p = chunk.GetValue<int64_t>(0);
    const int32_t m = chunk.GetValue<int32_t>(1);
    const int32_t n = chunk.GetValue<int32_t>(2);
    ++*rows_scanned;
    int64_t q = 0;
    if (!SolveQ(p, m, n, &q)) continue;
    if (q < opt.min_p || q > opt.max_p) continue;
    out->push_back(Edge{q, p, m, n});
  }
  *t_read = Seconds(t0);
  return true;
}

int RunBasis(const Options& opt) {
  scan::ScanPlanRequest request;
  request.select = {"m_k", "n_k"};

  std::vector<std::string> paths;
  double t_plan = 0.0;
  std::string error;
  if (!PlanPaths(opt, "partitions", request, &paths, &t_plan, &error)) {
    std::fprintf(stderr, "pp-graph: %s\n", error.c_str());
    return 1;
  }
  if (paths.empty()) {
    std::fprintf(stderr, "pp-graph: partitions has no data files\n");
    return 1;
  }

  duckdb::DBConfig cfg;
  cfg.SetOptionByName("threads", duckdb::Value::BIGINT(opt.threads));
  duckdb::DuckDB db(nullptr, &cfg);
  duckdb::Connection con(db);

  const std::string sql =
      "SELECT m_k, n_k, count(*) AS occurrences FROM read_parquet(" +
      QuoteList(paths) + ") GROUP BY m_k, n_k ORDER BY n_k, m_k";

  auto t0 = std::chrono::steady_clock::now();
  auto result = con.Query(sql);
  if (result->HasError()) {
    std::fprintf(stderr, "pp-graph: %s\n", result->GetError().c_str());
    return 1;
  }
  std::vector<Generator> gens;
  int64_t total_edges = 0;
  for (auto& row : *result) {
    Generator g;
    g.m = row.GetValue<int32_t>(0);
    g.n = row.GetValue<int32_t>(1);
    g.occurrences = row.GetValue<int64_t>(2);
    total_edges += g.occurrences;
    gens.push_back(std::move(g));
  }
  const double t_scan = Seconds(t0);

  GiNaC::symbol x("x");
  t0 = std::chrono::steady_clock::now();
  for (auto& g : gens) {
    bool ov = false;
    const int64_t c = IPow(2, g.m, &ov);
    if (ov) {
      std::fprintf(stderr, "pp-graph: 2^%d overflows int64\n", g.m);
      return 1;
    }
    const GiNaC::ex poly =
        GiNaC::pow(x, g.n) + GiNaC::numeric(static_cast<long>(c));
    g.hermite = primeparts::graph::ToHermite(poly, x);
  }
  std::vector<int> degrees;
  ReduceToBasis(&gens, &degrees);
  const double t_alg = Seconds(t0);

  size_t rank = 0;
  for (const auto& g : gens)
    if (g.pivot) ++rank;

  std::vector<std::string> names{"m",       "n",       "degree",
                                 "occurrences", "is_basis", "hermite"};
  std::vector<primeparts::query::MaterializeColumn> columns(names.size());
  for (size_t i = 0; i < names.size(); ++i) columns[i].name = names[i];
  columns[5].type = primeparts::query::ColumnType::kString;
  columns[5].no_stats = true;
  for (const auto& g : gens) {
    columns[0].ints.push_back(g.m);
    columns[1].ints.push_back(g.n);
    columns[2].ints.push_back(g.n);
    columns[3].ints.push_back(g.occurrences);
    columns[4].ints.push_back(g.pivot ? 1 : 0);
    std::string h = "{";
    bool first = true;
    for (const auto& [d, coef] : g.hermite) {
      if (!first) h += ",";
      first = false;
      std::ostringstream cs;
      cs << coef;
      h += "\"" + std::to_string(d) + "\":" + cs.str();
    }
    h += "}";
    columns[5].strings.push_back(std::move(h));
  }

  std::printf("pp-graph basis (whole dataset, unbounded)\n");
  std::printf("  partition rows aggregated : %" PRId64 "\n", total_edges);
  std::printf("  distinct (m, n) generators: %zu\n", gens.size());
  std::printf("  distinct Hermite degrees  : %zu\n", degrees.size());
  std::printf("  basis rank                : %zu\n", rank);
  std::printf("  scan %.3fs  plan %.3fs  algebra %.3fs\n", t_scan, t_plan,
              t_alg);

  if (opt.warehouse.empty()) {
    std::printf("\n(no --warehouse; not committing hermite_basis)\n");
    return 0;
  }
  std::string mode;
  auto catalog = ppc::OpenCatalog(opt.warehouse, opt.rest_uri, &mode, &error);
  if (!catalog) {
    std::fprintf(stderr, "pp-graph: OpenCatalog: %s\n", error.c_str());
    return 1;
  }
  const auto ns = ppc::ResolveNamespace(opt.ns_name);
  primeparts::query::MaterializeOptions mopt;
  mopt.sort_keys = {"n", "m"};
  std::string metadata_location;
  if (!primeparts::query::MaterializeColumns(catalog, ns, opt.warehouse,
                                             "hermite_basis", columns, mopt,
                                             &metadata_location, &error)) {
    std::fprintf(stderr, "pp-graph: MaterializeColumns: %s\n", error.c_str());
    return 1;
  }
  std::printf("\ncommitted %s.hermite_basis (%zu rows, unpartitioned)\n  %s\n",
              opt.ns_name.c_str(), gens.size(), metadata_location.c_str());
  return 0;
}

void EmitDot(const Poset& g,
             const std::vector<std::pair<int32_t, int32_t>>& cover,
             const std::vector<int32_t>& level) {
  std::printf("digraph hasse {\n  rankdir=BT;\n  node [shape=plaintext];\n");
  std::map<int32_t, std::vector<int32_t>> by_level;
  for (int32_t v = 0; v < static_cast<int32_t>(g.node.size()); ++v)
    by_level[level[v]].push_back(v);
  for (const auto& [lv, vs] : by_level) {
    std::printf("  { rank=same;");
    for (int32_t v : vs) std::printf(" n%" PRId64 ";", g.node[v]);
    std::printf(" }\n");
  }
  for (int32_t v = 0; v < static_cast<int32_t>(g.node.size()); ++v)
    std::printf("  n%" PRId64 " [label=\"%" PRId64 "\"];\n", g.node[v],
                g.node[v]);
  for (const auto& [a, b] : cover)
    std::printf("  n%" PRId64 " -> n%" PRId64 ";\n", g.node[a], g.node[b]);
  std::printf("}\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) {
    Usage(stderr);
    return 2;
  }

  if (opt.mode == "basis") return RunBasis(opt);
  if (opt.mode != "hasse") {
    std::fprintf(stderr, "unknown --mode %s (basis | hasse)\n", opt.mode.c_str());
    return 2;
  }

  std::vector<Edge> edges;
  int64_t rows_scanned = 0;
  double t_plan = 0.0;
  double t_read = 0.0;
  std::string error;
  if (!ReadEdges(opt, &edges, &rows_scanned, &t_plan, &t_read, &error)) {
    std::fprintf(stderr, "pp-graph: %s\n", error.c_str());
    return 1;
  }
  if (edges.empty()) {
    std::fprintf(stderr,
                 "pp-graph: no edges in [%" PRId64 ", %" PRId64
                 "] (scanned %" PRId64 " partition rows)\n",
                 opt.min_p, opt.max_p, rows_scanned);
    return 0;
  }

  const Poset g = BuildPoset(edges);
  const auto order = TopoOrder(g);
  if (order.size() != g.node.size()) {
    std::fprintf(stderr,
                 "pp-graph: edge relation is cyclic (%zu of %zu nodes ordered); "
                 "p = 2^m + q^n should be strictly increasing in q\n",
                 order.size(), g.node.size());
    return 1;
  }
  const auto level = MirskyLevels(g, order);
  const Closure closure = StrictReachability(g, order);
  const auto cover = CoverRelation(g, closure);
  int32_t matched = 0;
  const auto chains = ChainDecomposition(g, closure, &matched);
  const size_t dilworth_width = g.node.size() - static_cast<size_t>(matched);

  std::map<int32_t, std::vector<int32_t>> antichain;
  for (int32_t v = 0; v < static_cast<int32_t>(g.node.size()); ++v)
    antichain[level[v]].push_back(v);
  size_t width = 0;
  int32_t widest = 0;
  for (const auto& [lv, vs] : antichain) {
    if (vs.size() > width) {
      width = vs.size();
      widest = lv;
    }
  }
  size_t height = antichain.empty() ? 0 : antichain.rbegin()->first + 1;

  if (opt.format == "dot") {
    EmitDot(g, cover, level);
    return 0;
  }

  if (opt.format == "json") {
    std::printf("{\"window\":[%" PRId64 ",%" PRId64 "],", opt.min_p, opt.max_p);
    std::printf("\"nodes\":%zu,\"edges\":%zu,\"cover_edges\":%zu,",
                g.node.size(), edges.size(), cover.size());
    std::printf(
        "\"height\":%zu,\"width\":%zu,\"widest_level\":%zu,\"chains\":%zu}\n",
        height, dilworth_width, width, chains.size());
    return 0;
  }

  std::printf("pp-graph  window [%" PRId64 ", %" PRId64 "]\n", opt.min_p,
              opt.max_p);
  std::printf("  partition rows scanned : %" PRId64 "\n", rows_scanned);
  std::printf("  edges q -(m,n)-> p     : %zu\n", edges.size());
  std::printf("  poset nodes            : %zu\n", g.node.size());
  std::printf("  cover (Hasse) edges    : %zu\n", cover.size());
  std::printf("  height (longest chain) : %zu\n", height);
  std::printf("  width (Dilworth)       : %zu\n", dilworth_width);
  std::printf("  widest Mirsky level    : %zu  at level %d\n", width, widest);
  std::printf("  chains in decomposition: %zu%s\n", chains.size(),
              chains.size() == dilworth_width ? "  (= width, minimum)"
                                              : "  (NOT minimum)");
  std::printf("  plan %.3fs  read %.3fs\n\n", t_plan, t_read);

  std::printf("Hasse cover edges (first %" PRId64 "):\n", opt.top);
  int64_t shown = 0;
  for (const auto& [a, b] : cover) {
    if (shown++ >= opt.top) break;
    std::printf("  %12" PRId64 "  ->  %12" PRId64 "\n", g.node[a], g.node[b]);
  }

  std::printf("\nLongest chains (first %" PRId64 "):\n", opt.top);
  std::vector<std::vector<int32_t>> sorted = chains;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  shown = 0;
  for (const auto& c : sorted) {
    if (shown++ >= opt.top) break;
    std::printf("  [%zu] ", c.size());
    for (size_t i = 0; i < c.size(); ++i) {
      if (i) std::printf(" < ");
      std::printf("%" PRId64, g.node[c[i]]);
    }
    std::printf("\n");
  }

  std::printf("\nWidest antichain (level %d, %zu elements):\n", widest, width);
  const auto& wide = antichain[widest];
  shown = 0;
  for (int32_t v : wide) {
    if (shown++ >= opt.top) break;
    std::printf("  %" PRId64 "\n", g.node[v]);
  }
  if (static_cast<int64_t>(wide.size()) > opt.top)
    std::printf("  ... %zu more\n", wide.size() - opt.top);

  return 0;
}
