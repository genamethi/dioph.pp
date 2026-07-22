#include <duckdb.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/table_scan.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/rest_scan_plan.h"
#include "primeparts/client/session.h"
#include "primeparts/scan/scan_plan.h"

namespace client = primeparts::client;
namespace ppc = primeparts::catalog;
namespace scan = primeparts::scan;

namespace {

constexpr const char* kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";
constexpr const char* kDefaultRestUri = "http://127.0.0.1:8181";

struct Options {
  int64_t bound = 5000000000LL;
  int threads = 8;
  std::string rest_uri;
  std::string warehouse = kDefaultWarehouse;
};

struct Edge {
  int64_t p = 0;
  int64_t q = 0;
  int32_t m = 0;
  int32_t n = 0;
};

struct Word {
  int64_t a0 = 0;
  std::vector<std::pair<int32_t, int64_t>> segs;
  bool operator==(const Word& o) const {
    return a0 == o.a0 && segs == o.segs;
  }
};

struct Class {
  int64_t root = 0;
  Word word;
  bool operator==(const Class& o) const {
    return root == o.root && word == o.word;
  }
};

struct WordHash {
  size_t operator()(const Word& w) const {
    size_t h = std::hash<int64_t>{}(w.a0);
    for (const auto& [n, c] : w.segs) {
      h ^= (std::hash<int32_t>{}(n) + 0x9e3779b97f4a7c15ULL + (h << 6) +
            (h >> 2));
      h ^= (std::hash<int64_t>{}(c) + 0x9e3779b97f4a7c15ULL + (h << 6) +
            (h >> 2));
    }
    return h;
  }
};

struct ClassHash {
  size_t operator()(const Class& c) const {
    size_t h = std::hash<int64_t>{}(c.root);
    h ^= (WordHash{}(c.word) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    return h;
  }
};

using ClassMap = std::unordered_map<Class, int64_t, ClassHash>;

double Seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
      .count();
}

std::string StripFileScheme(const std::string& path) {
  if (path.rfind("file://", 0) == 0) return path.substr(7);
  if (path.rfind("file:", 0) == 0) return path.substr(5);
  return path;
}

std::string QuoteList(const std::vector<std::string>& paths) {
  std::string out = "[";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i) out += ", ";
    out += "'";
    for (char c : StripFileScheme(paths[i])) {
      if (c == '\'') out += "''";
      else out += c;
    }
    out += "'";
  }
  out += "]";
  return out;
}

Word WordPush(Word w, int32_t n, int64_t c) {
  if (n == 1) {
    if (!w.segs.empty()) w.segs.back().second += c;
    else w.a0 += c;
  } else {
    w.segs.emplace_back(n, c);
  }
  return w;
}

int64_t WordDegree(const Word& w) {
  int64_t d = 1;
  for (const auto& [n, c] : w.segs) d *= n;
  return d;
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
    } else {
      std::fprintf(stderr,
                   "usage: pp-graph [--bound N] [--threads N] "
                   "[--rest-uri URI] [--warehouse DIR]\n");
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
  request.filter = iceberg::Expressions::LessThanOrEqual(
      "p", iceberg::Literal::Long(opt.bound));

  const auto ns = ppc::ResolveNamespace("");
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  if (!ppc::PlanScanOnServer(opt.rest_uri, ns, "partitions", request,
                             *partitions.metadata(), ppc::PlanPollOptions{},
                             &tasks, &error)) {
    std::fprintf(stderr, "PlanScanOnServer: %s\n", error.c_str());
    return 1;
  }
  double t_plan = Seconds(t0);

  std::vector<std::string> paths;
  paths.reserve(tasks.size());
  for (const auto& task : tasks) paths.push_back(task->data_file()->file_path);
  if (paths.empty()) {
    std::printf("[wiring] no data files planned at B=%" PRId64 "\n", opt.bound);
    return 0;
  }

  duckdb::DBConfig config;
  config.SetOptionByName("threads", duckdb::Value::BIGINT(opt.threads));
  duckdb::DuckDB db(nullptr, &config);
  duckdb::Connection con(db);

  std::string sql =
      "SELECT p, m_k, n_k, q_k FROM read_parquet(" + QuoteList(paths) +
      ") WHERE p <= " + std::to_string(opt.bound);

  t0 = std::chrono::steady_clock::now();
  std::vector<Edge> edges;
  auto result = con.SendQuery(sql);
  if (result->HasError()) {
    std::fprintf(stderr, "duckdb: %s\n", result->GetError().c_str());
    return 1;
  }
  while (auto chunk = result->Fetch()) {
    duckdb::idx_t n = chunk->size();
    if (n == 0) break;
    auto* pv = duckdb::FlatVector::GetData<int64_t>(chunk->data[0]);
    auto* mv = duckdb::FlatVector::GetData<int32_t>(chunk->data[1]);
    auto* nv = duckdb::FlatVector::GetData<int32_t>(chunk->data[2]);
    auto* qv = duckdb::FlatVector::GetData<int64_t>(chunk->data[3]);
    for (duckdb::idx_t i = 0; i < n; ++i)
      edges.push_back({pv[i], qv[i], mv[i], nv[i]});
  }
  double t_read = Seconds(t0);

  std::printf("[wiring] files=%zu plan=%.2fs read=%.2fs edges=%zu\n",
              paths.size(), t_plan, t_read, edges.size());

  std::unordered_map<int64_t, std::vector<Edge>> parents;
  std::unordered_set<int64_t> has_children;
  std::unordered_set<int64_t> node_set;
  for (const auto& e : edges) {
    parents[e.p].push_back(e);
    has_children.insert(e.q);
    node_set.insert(e.p);
    node_set.insert(e.q);
  }
  std::vector<int64_t> nodes(node_set.begin(), node_set.end());
  std::sort(nodes.begin(), nodes.end());

  t0 = std::chrono::steady_clock::now();
  std::unordered_map<int64_t, ClassMap> memo;
  memo.reserve(nodes.size());
  for (int64_t v : nodes) {
    ClassMap res;
    auto it = parents.find(v);
    if (it == parents.end()) {
      res[Class{v, Word{}}] = 1;
    } else {
      for (const auto& e : it->second) {
        const int64_t c = int64_t{1} << e.m;
        auto pit = memo.find(e.q);
        if (pit == memo.end()) continue;
        for (const auto& [cls, cnt] : pit->second) {
          res[Class{cls.root, WordPush(cls.word, e.n, c)}] += cnt;
        }
      }
    }
    memo[v] = std::move(res);
  }
  double t_dp = Seconds(t0);

  std::unordered_set<Word, WordHash> distinct_words;
  int64_t maximal_classes = 0;
  int64_t maximal_chains = 0;
  int64_t max_degree = 0;
  const Word seed;
  for (int64_t v : nodes) {
    if (has_children.count(v)) continue;
    for (const auto& [cls, cnt] : memo[v]) {
      if (cls.word == seed) continue;
      ++maximal_classes;
      maximal_chains += cnt;
      distinct_words.insert(cls.word);
      max_degree = std::max(max_degree, WordDegree(cls.word));
    }
  }

  std::printf(
      "[collapse] maximal_classes=%" PRId64 " distinct_words=%zu "
      "maximal_chains=%" PRId64 " max_degree=%" PRId64 " dp=%.2fs\n",
      maximal_classes, distinct_words.size(), maximal_chains, max_degree,
      t_dp);
  return 0;
}
