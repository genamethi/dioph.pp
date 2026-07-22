#include <arrow/api.h>
#include <duckdb.hpp>
#include <ginac/ginac.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/table_scan.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/type.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/rest_scan_plan.h"
#include "primeparts/client/session.h"
#include "primeparts/query/materialize.h"
#include "primeparts/scan/scan_plan.h"
#include "primeparts/writer.h"

namespace client = primeparts::client;
namespace ppc = primeparts::catalog;
namespace ppq = primeparts::query;
namespace scan = primeparts::scan;

namespace {

constexpr const char* kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";
constexpr const char* kDefaultRestUri = "http://127.0.0.1:8181";

struct Options {
  int64_t bound = 5000000000LL;
  int threads = 8;
  bool materialize = false;
  std::string rest_uri;
  std::string warehouse = kDefaultWarehouse;
  std::string ns_name;
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
  bool operator<(const Word& o) const {
    if (a0 != o.a0) return a0 < o.a0;
    return segs < o.segs;
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

std::string JsonInts(const std::vector<int64_t>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ", ";
    s += std::to_string(v[i]);
  }
  s += "]";
  return s;
}

struct Features {
  int64_t degree = 1;
  std::string skeleton;
  std::string translations;
  std::string hermite;
  std::string symbolic;
};

Features WordFeatures(const Word& w, const GiNaC::symbol& x) {
  GiNaC::ex p = x + GiNaC::numeric(static_cast<long>(w.a0));
  std::vector<int64_t> sk;
  std::vector<int64_t> tr{w.a0};
  for (const auto& [n, c] : w.segs) {
    p = GiNaC::pow(p, n) + GiNaC::numeric(static_cast<long>(c));
    sk.push_back(n);
    tr.push_back(c);
  }
  auto hd = ToHermite(p, x);
  Features f;
  f.degree = WordDegree(w);
  f.skeleton = JsonInts(sk);
  f.translations = JsonInts(tr);
  std::string h = "{";
  bool first = true;
  for (const auto& [k, coef] : hd) {
    if (!first) h += ",";
    first = false;
    std::ostringstream cs;
    cs << coef;
    h += "\"" + std::to_string(k) + "\":" + cs.str();
  }
  h += "}";
  f.hermite = h;
  std::ostringstream ss;
  ss << GiNaC::expand(p);
  f.symbolic = ss.str();
  return f;
}

struct NodeClassWriter {
  std::shared_ptr<iceberg::Schema> schema;
  std::shared_ptr<arrow::Schema> aschema;
  std::unique_ptr<primeparts::BucketParquetWriter> writer;
  arrow::Int64Builder node, root, word, mult;
  int64_t buffered = 0;
  int64_t total = 0;

  bool Flush(std::string* err) {
    if (buffered == 0) return true;
    std::shared_ptr<arrow::Array> na, ra, wa, ma;
    if (!node.Finish(&na).ok() || !root.Finish(&ra).ok() ||
        !word.Finish(&wa).ok() || !mult.Finish(&ma).ok()) {
      if (err) *err = "node_classes: array finish failed";
      return false;
    }
    auto batch = arrow::RecordBatch::Make(aschema, buffered, {na, ra, wa, ma});
    buffered = 0;
    return writer->Write(*batch, err);
  }

  bool Add(int64_t n, int64_t r, int64_t w, int64_t m, std::string* err) {
    (void)node.Append(n);
    (void)root.Append(r);
    (void)word.Append(w);
    (void)mult.Append(m);
    ++buffered;
    ++total;
    if (buffered >= 65536) return Flush(err);
    return true;
  }
};

std::unique_ptr<NodeClassWriter> MakeNodeClassWriter(
    const std::string& warehouse, const iceberg::Namespace& ns,
    std::string* err) {
  auto w = std::make_unique<NodeClassWriter>();
  std::vector<iceberg::SchemaField> f;
  f.push_back(iceberg::SchemaField::MakeRequired(1, "node_id", iceberg::int64()));
  f.push_back(iceberg::SchemaField::MakeRequired(2, "root_id", iceberg::int64()));
  f.push_back(iceberg::SchemaField::MakeRequired(3, "word_id", iceberg::int64()));
  f.push_back(iceberg::SchemaField::MakeRequired(4, "mult", iceberg::int64()));
  w->schema = std::make_shared<iceberg::Schema>(std::move(f), 0);
  w->aschema = arrow::schema({arrow::field("node_id", arrow::int64()),
                              arrow::field("root_id", arrow::int64()),
                              arrow::field("word_id", arrow::int64()),
                              arrow::field("mult", arrow::int64())});
  primeparts::WriterConfig cfg;
  cfg.output_dir =
      primeparts::catalog::StagingDataDir(warehouse, ns, "node_classes");
  cfg.schema = w->schema;
  cfg.table_name = "node_classes";
  cfg.filename_prefix = "node_classes";
  cfg.partition_spec = iceberg::PartitionSpec::Unpartitioned();
  cfg.partition_values = std::make_shared<iceberg::PartitionValues>(
      std::vector<iceberg::Literal>{});
  cfg.stat_columns = {{"node_id", false}, {"root_id", false}, {"word_id", false}};
  cfg.simple_filename = true;
  cfg.target_rows_per_file = 4000000;
  w->writer = primeparts::BucketParquetWriter::Make(cfg, err);
  if (!w->writer) return nullptr;
  return w;
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
    } else if (a == "--namespace") {
      out->ns_name = need("--namespace");
    } else if (a == "--threads") {
      out->threads = std::atoi(need("--threads"));
    } else if (a == "--materialize") {
      out->materialize = true;
    } else {
      std::fprintf(stderr,
                   "usage: pp-graph [--bound N] [--threads N] [--materialize] "
                   "[--rest-uri URI] [--warehouse DIR] [--namespace NS]\n");
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

  const auto ns = ppc::ResolveNamespace(opt.ns_name);
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

  std::unordered_map<int64_t, int64_t> last_child;
  for (const auto& e : edges) {
    auto& lc = last_child[e.q];
    if (e.p > lc) lc = e.p;
  }

  t0 = std::chrono::steady_clock::now();
  std::unordered_map<int64_t, ClassMap> memo;
  std::priority_queue<std::pair<int64_t, int64_t>,
                      std::vector<std::pair<int64_t, int64_t>>,
                      std::greater<>>
      live;
  int64_t live_entries = 0;
  int64_t peak_entries = 0;

  const Word seed;
  std::unordered_map<Word, int64_t, WordHash> sink_word_id;
  auto intern_sink = [&](const Word& w) -> int64_t {
    auto f = sink_word_id.find(w);
    if (f != sink_word_id.end()) return f->second;
    int64_t id = static_cast<int64_t>(sink_word_id.size());
    sink_word_id.emplace(w, id);
    return id;
  };
  int64_t maximal_classes = 0;
  int64_t maximal_chains = 0;
  int64_t max_degree = 0;

  ppc::RestOptions ropts;
  ropts.rest_uri = opt.rest_uri;
  std::shared_ptr<iceberg::Catalog> catalog;
  std::unique_ptr<NodeClassWriter> ncw;
  if (opt.materialize) {
    std::string mode;
    catalog = ppc::MakeCatalog(ropts, opt.warehouse, &mode, &error);
    if (!catalog) {
      std::fprintf(stderr, "MakeCatalog: %s\n", error.c_str());
      return 1;
    }
    if (!ppc::DropTable(catalog, ns, opt.warehouse, "node_classes", true,
                        &error)) {
      std::fprintf(stderr, "DropTable node_classes: %s\n", error.c_str());
      return 1;
    }
    ncw = MakeNodeClassWriter(opt.warehouse, ns, &error);
    if (!ncw) {
      std::fprintf(stderr, "node_classes writer: %s\n", error.c_str());
      return 1;
    }
  }

  for (int64_t v : nodes) {
    while (!live.empty() && live.top().first < v) {
      int64_t u = live.top().second;
      live.pop();
      auto mit = memo.find(u);
      if (mit != memo.end()) {
        live_entries -= static_cast<int64_t>(mit->second.size());
        memo.erase(mit);
      }
    }

    ClassMap res;
    auto it = parents.find(v);
    if (it == parents.end()) {
      res[Class{v, seed}] = 1;
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

    if (has_children.count(v)) {
      live_entries += static_cast<int64_t>(res.size());
      peak_entries = std::max(peak_entries, live_entries);
      memo[v] = std::move(res);
      live.push({last_child[v], v});
    } else {
      for (const auto& [cls, cnt] : res) {
        if (cls.word == seed) continue;
        ++maximal_classes;
        maximal_chains += cnt;
        int64_t wid = intern_sink(cls.word);
        max_degree = std::max(max_degree, WordDegree(cls.word));
        if (ncw && !ncw->Add(v, cls.root, wid, cnt, &error)) {
          std::fprintf(stderr, "node_classes write: %s\n", error.c_str());
          return 1;
        }
      }
    }
  }
  double t_dp = Seconds(t0);

  std::printf(
      "[collapse] maximal_classes=%" PRId64 " distinct_words=%zu "
      "maximal_chains=%" PRId64 " max_degree=%" PRId64 " dp=%.2fs "
      "peak_live_entries=%" PRId64 "\n",
      maximal_classes, sink_word_id.size(), maximal_chains, max_degree, t_dp,
      peak_entries);

  if (!opt.materialize) return 0;

  auto t_pub = std::chrono::steady_clock::now();
  std::vector<Word> words(sink_word_id.size());
  for (const auto& [w, id] : sink_word_id) words[id] = w;

  GiNaC::symbol x("x");
  ppq::MaterializeColumn cw_id{"word_id", ppq::ColumnType::kLong, {}, {}, true};
  ppq::MaterializeColumn cw_deg{"degree", ppq::ColumnType::kLong, {}, {}, true};
  ppq::MaterializeColumn cw_sk{"skeleton", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_tr{"translations", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_he{"hermite", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_sym{"symbolic", ppq::ColumnType::kString, {}, {}, false};
  for (size_t i = 0; i < words.size(); ++i) {
    Features f = WordFeatures(words[i], x);
    cw_id.ints.push_back(static_cast<int64_t>(i));
    cw_deg.ints.push_back(f.degree);
    cw_sk.strings.push_back(f.skeleton);
    cw_tr.strings.push_back(f.translations);
    cw_he.strings.push_back(f.hermite);
    cw_sym.strings.push_back(f.symbolic);
  }
  std::string meta;
  std::vector<ppq::MaterializeColumn> cw{cw_id, cw_deg, cw_sk,
                                         cw_tr, cw_he, cw_sym};
  if (!ppq::MaterializeColumns(catalog, ns, opt.warehouse, "chain_words", cw,
                               &meta, &error)) {
    std::fprintf(stderr, "materialize chain_words: %s\n", error.c_str());
    return 1;
  }

  if (!ncw->Flush(&error)) {
    std::fprintf(stderr, "node_classes flush: %s\n", error.c_str());
    return 1;
  }
  std::vector<primeparts::WrittenFile> written;
  if (!ncw->writer->Close(&written, &error)) {
    std::fprintf(stderr, "node_classes close: %s\n", error.c_str());
    return 1;
  }
  std::vector<std::shared_ptr<iceberg::DataFile>> files;
  for (const auto& wf : written)
    if (wf.data_file) files.push_back(wf.data_file);
  if (!ppc::CommitFiles(catalog, ns, opt.warehouse, "node_classes", ncw->schema,
                        iceberg::PartitionSpec::Unpartitioned(),
                        ppc::TableDeclaration{}, files, &meta, &error)) {
    std::fprintf(stderr, "commit node_classes: %s\n", error.c_str());
    return 1;
  }
  std::printf(
      "[materialize] chain_words rows=%zu node_classes rows=%" PRId64
      " files=%zu pub=%.2fs\n",
      words.size(), ncw->total, files.size(), Seconds(t_pub));
  return 0;
}
