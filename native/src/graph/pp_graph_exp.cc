#include <arrow/api.h>
#include <ginac/ginac.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "primeparts/client/session.h"
#include "primeparts/scan/column_binder.h"
#include "primeparts/scan/scan_plan.h"

#include <flint/flint.h>
#include <flint/ulong_extras.h>

namespace client = primeparts::client;
namespace ppc = primeparts::catalog;
namespace scan = primeparts::scan;

namespace {

constexpr const char* kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";
constexpr const char* kDefaultRestUri = "http://127.0.0.1:8181";

struct PowerEdge {
  int64_t p = 0;
  int64_t q = 0;
  int32_t m = 0;
  int32_t n = 0;
};

struct Options {
  int64_t bound = 5000000000LL;
  int threads = 8;
  int64_t batch = 0;
  std::string rest_uri;
  std::string warehouse = kDefaultWarehouse;
};

double Seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
      .count();
}

int64_t PowCapped(int64_t q, int32_t n, int64_t cap) {
  int64_t v = 1;
  for (int32_t i = 0; i < n; ++i) {
    if (v > cap / q) return -1;
    v *= q;
  }
  return v;
}

GiNaC::ex He(int n, const GiNaC::symbol& x) {
  GiNaC::ex a = 1;
  GiNaC::ex b = x;
  if (n == 0) return a;
  for (int k = 1; k < n; ++k) {
    GiNaC::ex c = GiNaC::expand(x * b - k * a);
    a = b;
    b = c;
  }
  return b;
}

std::map<int, GiNaC::ex> ToHermite(GiNaC::ex P, const GiNaC::symbol& x) {
  std::map<int, GiNaC::ex> out;
  P = GiNaC::expand(P);
  while (!P.is_zero()) {
    int d = P.degree(x);
    GiNaC::ex c = P.lcoeff(x);
    out[d] = c;
    P = GiNaC::expand(P - c * He(d, x));
    if (d == 0) break;
  }
  return out;
}

std::string HermiteString(const std::map<int, GiNaC::ex>& hd) {
  std::string s;
  for (auto it = hd.rbegin(); it != hd.rend(); ++it) {
    std::ostringstream term;
    term << it->second;
    if (!s.empty()) s += " + ";
    if (it->first == 0) {
      s += term.str();
    } else {
      s += term.str() + "*He_" + std::to_string(it->first);
    }
  }
  return s;
}

bool GinacSelfTest() {
  GiNaC::symbol x("x");
  GiNaC::ex known =
      GiNaC::expand(GiNaC::pow(GiNaC::pow(x, 2) + 2, 2) + 16);
  if (!(known - GiNaC::expand(GiNaC::pow(x, 4) + 4 * GiNaC::pow(x, 2) + 20))
           .is_zero())
    return false;
  auto hd = ToHermite(known, x);
  if (hd.size() != 3) return false;
  if (!(hd[4] - 1).is_zero() || !(hd[2] - 10).is_zero() ||
      !(hd[0] - 27).is_zero())
    return false;
  return (known.subs(x == 3) - 137).is_zero();
}

bool IsPrimeU64(int64_t v) {
  return v > 1 && n_is_prime(static_cast<ulong>(v));
}

struct Reach {
  std::set<int64_t> sources;
  std::unordered_map<int64_t, std::pair<int64_t, int32_t>> parent;
};

Reach TranslationReach(int64_t start, int64_t cap,
                       const std::set<int64_t>& targets) {
  Reach r;
  std::unordered_set<int64_t> visited;
  std::vector<int64_t> stack{start};
  visited.insert(start);
  while (!stack.empty()) {
    int64_t v = stack.back();
    stack.pop_back();
    for (int32_t m = 1; m < 62; ++m) {
      int64_t step = int64_t{1} << m;
      if (step > cap - v) break;
      int64_t s = v + step;
      if (visited.count(s) || !IsPrimeU64(s)) continue;
      visited.insert(s);
      r.parent[s] = {v, m};
      if (targets.count(s)) r.sources.insert(s);
      stack.push_back(s);
    }
  }
  return r;
}

bool ParseArgs(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--bound") {
      out->bound = static_cast<int64_t>(std::strtod(need("--bound"), nullptr));
    } else if (a == "--rest-uri") {
      out->rest_uri = need("--rest-uri");
    } else if (a == "--warehouse") {
      out->warehouse = need("--warehouse");
    } else if (a == "--threads") {
      out->threads = std::atoi(need("--threads"));
    } else if (a == "--batch") {
      out->batch = std::atoll(need("--batch"));
    } else {
      std::fprintf(stderr,
                   "usage: pp-graph-exp [--bound N] [--threads N] "
                   "[--batch N] [--rest-uri URI] [--warehouse DIR]\n");
      return false;
    }
  }
  if (out->rest_uri.empty()) {
    const char* env = std::getenv("PRIMEPARTS_REST_URI");
    out->rest_uri = env ? env : kDefaultRestUri;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) return 2;

  std::string error;
  client::SessionOptions session_options;
  session_options.rest_uri = opt.rest_uri;
  session_options.warehouse = opt.warehouse;
  session_options.scan_threads = opt.threads;
  session_options.read_batch_size = opt.batch;
  auto session = client::Session::Open(session_options, &error);
  if (!session) {
    std::fprintf(stderr, "Session::Open: %s\n", error.c_str());
    return 1;
  }

  client::TableHandle partitions;
  if (!session->LoadTable("partitions", &partitions, &error)) {
    std::fprintf(stderr, "LoadTable: %s\n", error.c_str());
    return 1;
  }

  scan::ScanPlanRequest request;
  request.select = {"p", "m_k", "n_k", "q_k"};
  request.filter = iceberg::Expressions::And(
      iceberg::Expressions::GreaterThanOrEqual("n_k", iceberg::Literal::Int(2)),
      iceberg::Expressions::LessThanOrEqual("p",
                                            iceberg::Literal::Long(opt.bound)));

  auto t0 = std::chrono::steady_clock::now();
  auto stream = session->Scan(partitions, request, &error);
  if (!stream) {
    std::fprintf(stderr, "Scan: %s\n", error.c_str());
    return 1;
  }
  double t_plan = Seconds(t0);

  t0 = std::chrono::steady_clock::now();
  std::vector<PowerEdge> edges;
  std::shared_ptr<arrow::RecordBatch> batch;
  while (stream->Next(&batch, &error)) {
    if (!batch) break;
    const int64_t* pa = scan::BindInt64(*batch, "p", &error);
    const int32_t* ma = scan::BindInt32(*batch, "m_k", &error);
    const int32_t* na = scan::BindInt32(*batch, "n_k", &error);
    const int64_t* qa = scan::BindInt64(*batch, "q_k", &error);
    if (!pa || !ma || !na || !qa) {
      std::fprintf(stderr, "column bind: %s\n", error.c_str());
      return 1;
    }
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      edges.push_back({pa[i], qa[i], ma[i], na[i]});
    }
  }
  if (!error.empty()) {
    std::fprintf(stderr, "read: %s\n", error.c_str());
    return 1;
  }
  double t_read = Seconds(t0);

  std::printf(
      "[wiring] planned_via=%s files=%" PRId64 " shards=%d planned_rows=%" PRId64
      " plan=%.2fs read=%.2fs\n",
      stream->planned_via() == ppc::ScanPlanningMode::kServer ? "server"
                                                              : "client",
      stream->file_count(), stream->shard_count(), stream->planned_rows(),
      t_plan, t_read);

  int64_t bad = 0;
  for (const auto& e : edges) {
    int64_t qn = PowCapped(e.q, e.n, opt.bound);
    if (qn < 0 || e.p != (int64_t{1} << e.m) + qn) ++bad;
  }
  std::map<int32_t, int64_t> grading;
  for (const auto& e : edges) ++grading[e.n];
  int32_t max_n = grading.empty() ? 1 : grading.rbegin()->first;
  int32_t predicted = static_cast<int32_t>(
      std::floor(std::log(static_cast<double>(opt.bound)) / std::log(3.0)));

  std::printf("[census] power_edges=%zu identity_bad=%" PRId64
              " max_n=%d floor(log3 B)=%d\n",
              edges.size(), bad, max_n, predicted);
  for (const auto& [n, c] : grading)
    std::printf("[census] n=%d edges=%" PRId64 "\n", n, c);

  std::printf("[ginac] self_test=%s\n", GinacSelfTest() ? "ok" : "FAILED");

  if (edges.empty()) {
    std::printf("[composite] no power edges at B=%" PRId64 "\n", opt.bound);
    return 0;
  }

  std::sort(edges.begin(), edges.end(),
            [](const PowerEdge& a, const PowerEdge& b) { return a.p < b.p; });
  std::set<int64_t> source_set;
  for (const auto& e : edges) source_set.insert(e.q);
  const int64_t source_cap = *source_set.rbegin();

  int64_t direct = 0;
  for (const auto& e : edges)
    if (source_set.count(e.p)) ++direct;

  struct Witness {
    PowerEdge first;
    PowerEdge second;
    int64_t start = 0;
  };
  std::unordered_map<int64_t, std::set<int64_t>> words_at;
  std::unordered_map<int64_t, Reach> reach_from;
  std::map<int64_t, int64_t> composite_hits;
  std::optional<Witness> witness;

  auto first_power_edge_at = [&](int64_t endpoint) -> const PowerEdge* {
    for (const auto& f : edges)
      if (f.p == endpoint) return &f;
    return nullptr;
  };

  for (const auto& e : edges) {
    std::set<int64_t> feeder;
    auto take = [&](int64_t endpoint) {
      auto it = words_at.find(endpoint);
      if (it == words_at.end()) return;
      feeder.insert(it->second.begin(), it->second.end());
      if (!witness) {
        const PowerEdge* f = first_power_edge_at(endpoint);
        if (f) witness = Witness{*f, e, endpoint};
      }
    };
    take(e.q);
    for (const auto& [start, reach] : reach_from)
      if (start != e.q && reach.sources.count(e.q)) take(start);

    std::set<int64_t>& slot = words_at[e.p];
    slot.insert(e.n);
    for (int64_t d : feeder) {
      slot.insert(d * e.n);
      ++composite_hits[d * e.n];
    }
    if (e.p <= source_cap && !reach_from.count(e.p))
      reach_from.emplace(e.p, TranslationReach(e.p, source_cap, source_set));
  }

  std::printf("[composite] direct_concatenations=%" PRId64
              " reach_starts=%zu source_cap=%" PRId64 "\n",
              direct, reach_from.size(), source_cap);
  if (composite_hits.empty()) {
    std::printf("[composite] no composite-degree word realized at B=%" PRId64
                "\n",
                opt.bound);
  } else {
    for (const auto& [d, c] : composite_hits)
      std::printf("[composite] product_degree=%" PRId64
                  " edge_feeder_pairs=%" PRId64 "\n",
                  d, c);
  }

  if (witness) {
    GiNaC::symbol x("x");
    const Witness& w = *witness;
    int64_t translation = 0;
    if (w.first.p != w.second.q) {
      translation = -1;
      auto rit = reach_from.find(w.first.p);
      if (rit != reach_from.end()) {
        translation = 0;
        int64_t node = w.second.q;
        while (node != w.first.p) {
          auto pit = rit->second.parent.find(node);
          if (pit == rit->second.parent.end()) {
            translation = -1;
            break;
          }
          translation += int64_t{1} << pit->second.second;
          node = pit->second.first;
        }
      }
    }
    if (translation >= 0) {
      GiNaC::ex word =
          GiNaC::pow(GiNaC::pow(x, w.first.n) +
                         GiNaC::numeric(static_cast<long>(
                             (int64_t{1} << w.first.m) + translation)),
                     w.second.n) +
          GiNaC::numeric(static_cast<long>(int64_t{1} << w.second.m));
      auto hd = ToHermite(word, x);
      std::ostringstream poly;
      poly << GiNaC::expand(word);
      std::printf("[witness] root=%" PRId64 " --(m=%d,n=%d)--> %" PRId64
                  " --(+%" PRId64 ")--> %" PRId64 " --(m=%d,n=%d)--> %" PRId64
                  "\n",
                  w.first.q, w.first.m, w.first.n, w.first.p, translation,
                  w.second.q, w.second.m, w.second.n, w.second.p);
      std::printf("[witness] poly=%s\n", poly.str().c_str());
      std::printf("[witness] hermite=%s\n", HermiteString(hd).c_str());
      GiNaC::ex value = word.subs(x == GiNaC::numeric(static_cast<long>(w.first.q)));
      std::ostringstream val;
      val << value;
      std::printf("[witness] value_at_root=%s expected=%" PRId64 "\n",
                  val.str().c_str(), w.second.p);
    }
  }

  return 0;
}
