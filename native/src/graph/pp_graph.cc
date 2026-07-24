#include <arrow/api.h>
#include <duckdb.hpp>
#include <ginac/ginac.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
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
#include "primeparts/catalog/pp_commit.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/rest_scan_plan.h"
#include "primeparts/client/session.h"
#include "primeparts/graph/pp_graph_store.h"
#include "primeparts/query/materialize.h"
#include "primeparts/scan/scan_plan.h"
#include "primeparts/schemas.h"
#include "primeparts/writer.h"

namespace client = primeparts::client;
namespace ppc = primeparts::catalog;
namespace ppq = primeparts::query;
namespace scan = primeparts::scan;

namespace {

using namespace primeparts::graph;

constexpr const char* kDefaultWarehouse =
    "/media/extssd/research/dioph.pp/data/ib-staging";
constexpr const char* kDefaultRestUri = "http://127.0.0.1:8181";

struct Options {
  int64_t bound = 5000000000LL;
  int threads = 8;
  int workers = 1;
  int slices = 0;
  int cone_probe = 0;
  int64_t slice_width = 1000000;
  bool materialize = false;
  bool sweep_only = false;
  bool stream = false;
  std::string rest_uri;
  std::string warehouse = kDefaultWarehouse;
  std::string ns_name;
  std::string spill_dir;
};

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

// Parallel topological sweep (Kahn). A node is ready once every distinct parent
// is computed; workers pull ready nodes, merge parents' word-id sets through the
// shared ConcStore, publish memo[v], then release children. qmu is the single
// synchronization point (guards ready/indeg/memo publication), so parent memos
// are always visible before a child is popped.
ParResult RunParallel(
    const std::vector<int64_t>& nodes,
    const std::unordered_map<int64_t, std::vector<Edge>>& parents,
    const std::unordered_set<int64_t>& has_children, int workers) {
  const int32_t N = static_cast<int32_t>(nodes.size());
  std::unordered_map<int64_t, int32_t> idx;
  idx.reserve(N);
  for (int32_t i = 0; i < N; ++i) idx[nodes[i]] = i;

  std::vector<std::vector<int32_t>> children(N);
  std::vector<int> indeg(N, 0);
  for (int32_t vi = 0; vi < N; ++vi) {
    auto it = parents.find(nodes[vi]);
    if (it == parents.end()) continue;
    std::unordered_set<int64_t> pq;
    for (const auto& e : it->second) pq.insert(e.q);
    indeg[vi] = static_cast<int>(pq.size());
    for (int64_t q : pq) children[idx[q]].push_back(vi);
  }

  ConcStore store;
  std::vector<std::vector<int32_t>> memo(N);

  std::mutex qmu;
  std::condition_variable cv;
  std::vector<int32_t> ready;
  int64_t remaining = N;
  bool done = false;
  for (int32_t i = 0; i < N; ++i)
    if (indeg[i] == 0) ready.push_back(i);

  std::mutex omu;
  std::unordered_map<int32_t, int64_t> sink_out;
  int64_t pairs = 0, maxdeg = 0;

  auto worker = [&]() {
    while (true) {
      int32_t v;
      {
        std::unique_lock<std::mutex> lk(qmu);
        cv.wait(lk, [&] { return !ready.empty() || done; });
        if (ready.empty()) return;
        v = ready.back();
        ready.pop_back();
      }
      const int64_t vv = nodes[v];
      std::unordered_set<int32_t> res;
      auto it = parents.find(vv);
      if (it == parents.end()) {
        res.insert(store.seed);
      } else {
        for (const auto& e : it->second) {
          const int64_t c = int64_t{1} << e.m;
          for (int32_t w : memo[idx[e.q]]) res.insert(store.Push(w, e.n, c));
        }
      }
      if (has_children.count(vv)) {
        memo[v].assign(res.begin(), res.end());
      } else {
        std::lock_guard<std::mutex> g(omu);
        for (int32_t w : res) {
          if (w == store.seed) continue;
          ++pairs;
          int64_t d = store.Degree(w);
          if (d > maxdeg) maxdeg = d;
          sink_out.emplace(w, static_cast<int64_t>(sink_out.size()));
        }
      }
      {
        std::lock_guard<std::mutex> lk(qmu);
        for (int32_t c : children[v])
          if (--indeg[c] == 0) ready.push_back(c);
        if (--remaining == 0) done = true;
        cv.notify_all();
      }
    }
  };

  std::vector<std::thread> pool;
  for (int i = 0; i < workers; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  ParResult r;
  r.node_word_pairs = pairs;
  r.distinct_words = static_cast<int64_t>(sink_out.size());
  r.max_degree = maxdeg;
  return r;
}

// In-memory validation of the sliced connection algebra: process nodes in
// p-order, but treat a parent in a strictly earlier slice as a connection (defer
// it) instead of pushing its memo. Then reassemble by expansion. Must reproduce
// the single-pass counts for any K (the memo is still fully held here; the disk
// dump/reload that actually bounds RAM is the next step).
ParResult RunSliced(
    const std::vector<int64_t>& nodes,
    const std::unordered_map<int64_t, std::vector<Edge>>& parents,
    const std::unordered_set<int64_t>& has_children, int K, int64_t bound) {
  WordStore store;
  std::unordered_map<int64_t, std::vector<MemoEntry>> memo;
  auto slice_of = [&](int64_t p) {
    int s = static_cast<int>((static_cast<__int128>(p) * K) / (bound + 1));
    return s < 0 ? 0 : (s >= K ? K - 1 : s);
  };

  for (int64_t v : nodes) {
    std::unordered_set<MemoEntry, MemoEntryHash> res;
    auto it = parents.find(v);
    if (it == parents.end()) {
      res.insert({store.seed, -1});
    } else {
      const int vs = slice_of(v);
      for (const auto& e : it->second) {
        const int64_t c = int64_t{1} << e.m;
        if (slice_of(e.q) < vs) {
          res.insert({store.Push(store.seed, e.n, c), e.q});
        } else {
          auto pit = memo.find(e.q);
          if (pit == memo.end()) continue;
          for (const auto& me : pit->second)
            res.insert({store.Push(me.word, e.n, c), me.conn});
        }
      }
    }
    memo[v].assign(res.begin(), res.end());
  }

  ParResult r;
  std::unordered_set<int32_t> distinct;
  for (int64_t v : nodes) {
    if (has_children.count(v)) continue;
    std::unordered_set<int32_t> complete;
    for (const auto& me : memo[v]) {
      if (me.word == store.seed && me.conn < 0) continue;
      Expand(store, memo, me.word, me.conn, complete);
    }
    r.node_word_pairs += static_cast<int64_t>(complete.size());
    for (int32_t w : complete) {
      distinct.insert(w);
      int64_t d = store.Degree(w);
      if (d > r.max_degree) r.max_degree = d;
    }
  }
  r.distinct_words = static_cast<int64_t>(distinct.size());
  return r;
}

// Parallel sliced sweep. A node is ready once its *in-slice* parents are done
// (parents in an earlier slice are connections, always available), so the Kahn
// sweep parallelizes within a slice through the concurrent store. memo entries
// carry (word, conn); below-slice parents defer as connections. In-memory
// validation form: the whole memo is held and reassembled here; the per-slice
// disk dump/reload that bounds RAM is the next step.
ParResult RunSlicedParallel(
    const std::vector<int64_t>& nodes,
    const std::unordered_map<int64_t, std::vector<Edge>>& parents,
    const std::unordered_set<int64_t>& has_children, int64_t slice_width,
    int workers) {
  const int32_t N = static_cast<int32_t>(nodes.size());
  std::unordered_map<int64_t, int32_t> idx;
  idx.reserve(N);
  for (int32_t i = 0; i < N; ++i) idx[nodes[i]] = i;
  auto slice_of = [&](int64_t p) { return p / slice_width; };

  std::vector<std::vector<int32_t>> children(N);
  std::vector<std::atomic<int>> indeg(N);
  for (int32_t i = 0; i < N; ++i) indeg[i].store(0, std::memory_order_relaxed);
  for (int32_t vi = 0; vi < N; ++vi) {
    auto it = parents.find(nodes[vi]);
    if (it == parents.end()) continue;
    const int64_t vs = slice_of(nodes[vi]);
    std::unordered_set<int64_t> inslice;
    for (const auto& e : it->second)
      if (slice_of(e.q) == vs) inslice.insert(e.q);
    indeg[vi].store(static_cast<int>(inslice.size()), std::memory_order_relaxed);
    for (int64_t q : inslice) children[idx[q]].push_back(vi);
  }

  ConcStore store;
  std::vector<std::vector<MemoEntry>> memo(N);

  std::mutex qmu;
  std::condition_variable cv;
  std::vector<int32_t> ready;
  std::atomic<int64_t> remaining{N};
  std::atomic<bool> done{false};
  for (int32_t i = 0; i < N; ++i)
    if (indeg[i].load(std::memory_order_relaxed) == 0) ready.push_back(i);

  auto worker = [&]() {
    while (true) {
      int32_t v;
      {
        std::unique_lock<std::mutex> lk(qmu);
        cv.wait(lk, [&] { return !ready.empty() || done.load(); });
        if (ready.empty()) return;
        v = ready.back();
        ready.pop_back();
      }
      const int64_t vv = nodes[v];
      const int64_t vs = slice_of(vv);
      std::unordered_set<MemoEntry, MemoEntryHash> res;
      auto it = parents.find(vv);
      if (it == parents.end()) {
        res.insert({store.seed, -1});
      } else {
        for (const auto& e : it->second) {
          const int64_t c = int64_t{1} << e.m;
          if (slice_of(e.q) < vs) {
            res.insert({store.Push(store.seed, e.n, c), e.q});
          } else {
            for (const auto& me : memo[idx[e.q]])
              res.insert({store.Push(me.word, e.n, c), me.conn});
          }
        }
      }
      memo[v].assign(res.begin(), res.end());
      {
        std::lock_guard<std::mutex> lk(qmu);
        for (int32_t c : children[v])
          if (indeg[c].fetch_sub(1, std::memory_order_acq_rel) == 1)
            ready.push_back(c);
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
          done.store(true);
        cv.notify_all();
      }
    }
  };
  std::vector<std::thread> pool;
  for (int i = 0; i < workers; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();

  std::unordered_map<int64_t, std::vector<MemoEntry>> flat;
  flat.reserve(N);
  for (int32_t i = 0; i < N; ++i) flat[nodes[i]] = memo[i];

  ParResult r;
  std::unordered_set<int32_t> distinct;
  for (int64_t v : nodes) {
    if (has_children.count(v)) continue;
    std::unordered_set<int32_t> complete;
    for (const auto& me : memo[idx[v]]) {
      if (me.word == store.seed && me.conn < 0) continue;
      Expand(store, flat, me.word, me.conn, complete);
    }
    r.node_word_pairs += static_cast<int64_t>(complete.size());
    for (int32_t w : complete) {
      distinct.insert(w);
      int64_t d = store.Degree(w);
      if (d > r.max_degree) r.max_degree = d;
    }
  }
  r.distinct_words = static_cast<int64_t>(distinct.size());
  return r;
}

struct NodeClassWriter {
  std::shared_ptr<iceberg::Schema> schema;
  std::shared_ptr<arrow::Schema> aschema;
  std::unique_ptr<primeparts::BucketParquetWriter> writer;
  arrow::Int64Builder node, word;
  int64_t buffered = 0;
  int64_t total = 0;

  bool Flush(std::string* err) {
    if (buffered == 0) return true;
    std::shared_ptr<arrow::Array> na, wa;
    if (!node.Finish(&na).ok() || !word.Finish(&wa).ok()) {
      if (err) *err = "node_classes: array finish failed";
      return false;
    }
    auto batch = arrow::RecordBatch::Make(aschema, buffered, {na, wa});
    buffered = 0;
    return writer->Write(*batch, err);
  }

  bool Add(int64_t n, int64_t w, std::string* err) {
    (void)node.Append(n);
    (void)word.Append(w);
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
  f.push_back(iceberg::SchemaField::MakeRequired(2, "word_id", iceberg::int64()));
  w->schema = std::make_shared<iceberg::Schema>(std::move(f), 0);
  w->aschema = arrow::schema({arrow::field("node_id", arrow::int64()),
                              arrow::field("word_id", arrow::int64())});
  primeparts::WriterConfig cfg;
  cfg.output_dir =
      primeparts::catalog::StagingDataDir(warehouse, ns, "node_classes");
  cfg.schema = w->schema;
  cfg.table_name = "node_classes";
  cfg.filename_prefix = "node_classes";
  cfg.partition_spec = iceberg::PartitionSpec::Unpartitioned();
  cfg.partition_values = std::make_shared<iceberg::PartitionValues>(
      std::vector<iceberg::Literal>{});
  cfg.stat_columns = {{"node_id", false}, {"word_id", false}};
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
    } else if (a == "--workers") {
      out->workers = std::atoi(need("--workers"));
    } else if (a == "--slices") {
      out->slices = std::atoi(need("--slices"));
    } else if (a == "--slice-width") {
      out->slice_width =
          static_cast<int64_t>(std::strtod(need("--slice-width"), nullptr));
    } else if (a == "--spill-dir") {
      out->spill_dir = need("--spill-dir");
    } else if (a == "--cone-probe") {
      out->cone_probe = std::atoi(need("--cone-probe"));
    } else if (a == "--materialize") {
      out->materialize = true;
    } else if (a == "--sweep-only") {
      out->sweep_only = true;
    } else if (a == "--stream") {
      out->stream = true;
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

int PublishFromSpill(const Options& opt, const iceberg::Namespace& ns,
                     int64_t num_slices,
                     const std::unordered_set<int64_t>& has_children,
                     const std::unordered_map<int64_t, int64_t>& last_ref_slice,
                     double t_sweep) {
  std::string error;
  auto t0 = std::chrono::steady_clock::now();

  if (opt.materialize) {
    SkeletonDict dict;
    CoproductColumns cc;
    for (int64_t s = 0; s < num_slices; ++s) {
      if (!BuildCoproductSlice(opt.spill_dir, s, &dict, &cc, &error)) {
        std::fprintf(stderr, "coproduct: %s\n", error.c_str());
        return 1;
      }
    }
    double t_build = Seconds(t0);
    auto t_pub = std::chrono::steady_clock::now();

    ppc::RestOptions ropts;
    ropts.rest_uri = opt.rest_uri;
    std::string mode;
    auto catalog = ppc::MakeCatalog(ropts, opt.warehouse, &mode, &error);
    if (!catalog) {
      std::fprintf(stderr, "MakeCatalog: %s\n", error.c_str());
      return 1;
    }

    auto to_i64 = [](const std::vector<int32_t>& v) {
      return std::vector<int64_t>(v.begin(), v.end());
    };
    std::string meta;
    const int64_t gib = int64_t{1} << 30;

    std::vector<ppq::MaterializeColumn> wcols{
        {"word_id", ppq::ColumnType::kLong, cc.word_id, {}, false},
        {"a0", ppq::ColumnType::kLong, cc.a0, {}, false},
        {"skeleton_id", ppq::ColumnType::kInt, to_i64(cc.skeleton_id), {}, false},
        {"c1", ppq::ColumnType::kLong, cc.c[0], {}, false},
        {"c2", ppq::ColumnType::kLong, cc.c[1], {}, false},
        {"c3", ppq::ColumnType::kLong, cc.c[2], {}, false},
        {"c4", ppq::ColumnType::kLong, cc.c[3], {}, false},
        {"slice", ppq::ColumnType::kLong, cc.word_slice, {}, false}};
    ppq::MaterializeOptions wopts;
    wopts.sort_keys = {"word_id"};
    wopts.target_file_bytes = gib;
    if (!ppq::MaterializeColumns(catalog, ns, opt.warehouse, "words", wcols,
                                 wopts, &meta, &error)) {
      std::fprintf(stderr, "materialize words: %s\n", error.c_str());
      return 1;
    }

    const size_t nw = cc.nw_node.size();
    std::vector<size_t> ord(nw);
    for (size_t i = 0; i < nw; ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
      if (cc.nw_node[a] != cc.nw_node[b]) return cc.nw_node[a] < cc.nw_node[b];
      return cc.nw_word[a] < cc.nw_word[b];
    });
    std::vector<int64_t> nwn(nw), nww(nw), nwc(nw);
    for (size_t i = 0; i < nw; ++i) {
      nwn[i] = cc.nw_node[ord[i]];
      nww[i] = cc.nw_word[ord[i]];
      nwc[i] = cc.nw_conn[ord[i]];
    }
    std::vector<ppq::MaterializeColumn> ncols{
        {"node_id", ppq::ColumnType::kLong, nwn, {}, false},
        {"word_id", ppq::ColumnType::kLong, nww, {}, false},
        {"conn", ppq::ColumnType::kLong, nwc, {}, false}};
    ppq::MaterializeOptions nopts;
    nopts.sort_keys = {"node_id"};
    nopts.target_file_bytes = gib;
    if (!ppq::MaterializeColumns(catalog, ns, opt.warehouse, "node_words", ncols,
                                 nopts, &meta, &error)) {
      std::fprintf(stderr, "materialize node_words: %s\n", error.c_str());
      return 1;
    }

    std::vector<int64_t> sk_id, sk_n[kMaxSegments], sk_len;
    for (size_t i = 0; i < dict.tuples.size(); ++i) {
      sk_id.push_back(static_cast<int64_t>(i));
      const auto& t = dict.tuples[i];
      sk_len.push_back(static_cast<int64_t>(t.size()));
      for (int j = 0; j < kMaxSegments; ++j)
        sk_n[j].push_back(j < static_cast<int>(t.size()) ? t[j] : 0);
    }
    std::vector<ppq::MaterializeColumn> scols{
        {"skeleton_id", ppq::ColumnType::kInt, sk_id, {}, false},
        {"n1", ppq::ColumnType::kInt, sk_n[0], {}, false},
        {"n2", ppq::ColumnType::kInt, sk_n[1], {}, false},
        {"n3", ppq::ColumnType::kInt, sk_n[2], {}, false},
        {"n4", ppq::ColumnType::kInt, sk_n[3], {}, false},
        {"len", ppq::ColumnType::kInt, sk_len, {}, false}};
    ppq::MaterializeOptions sopts;
    sopts.sort_keys = {"skeleton_id"};
    if (!ppq::MaterializeColumns(catalog, ns, opt.warehouse, "skeletons", scols,
                                 sopts, &meta, &error)) {
      std::fprintf(stderr, "materialize skeletons: %s\n", error.c_str());
      return 1;
    }

    std::printf(
        "[coproduct] words=%zu node_words=%zu skeletons=%zu sweep=%.2fs "
        "build=%.2fs slices=%" PRId64 " width=%" PRId64 " pub=%.2fs\n",
        cc.word_id.size(), nw, dict.tuples.size(), t_sweep, t_build, num_slices,
        opt.slice_width, Seconds(t_pub));
    return 0;
  }

  ParResult r = ReassembleFromDisk(opt.spill_dir, num_slices, has_children,
                                   last_ref_slice, &error);
  if (!error.empty()) {
    std::fprintf(stderr, "reassemble: %s\n", error.c_str());
    return 1;
  }
  std::printf(
      "[collapse] node_word_pairs=%" PRId64 " distinct_words=%" PRId64
      " max_degree=%" PRId64 " sweep=%.2fs reassemble=%.2fs slices=%" PRId64
      " width=%" PRId64 " disk\n",
      r.node_word_pairs, r.distinct_words, r.max_degree, t_sweep, Seconds(t0),
      num_slices, opt.slice_width);
  return 0;
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

  if (opt.stream) {
    if (opt.spill_dir.empty()) {
      std::fprintf(stderr, "--stream requires --spill-dir\n");
      return 1;
    }
    client::TableHandle primes;
    if (!session->LoadTable("primes", &primes, &error)) {
      std::fprintf(stderr, "LoadTable primes: %s\n", error.c_str());
      return 1;
    }
    scan::ScanPlanRequest preq;
    preq.select = {"p", "k"};
    preq.filter = iceberg::Expressions::LessThanOrEqual(
        "p", iceberg::Literal::Long(opt.bound));
    std::vector<std::shared_ptr<iceberg::FileScanTask>> ptasks;
    if (!ppc::PlanScanOnServer(opt.rest_uri, ns, "primes", preq,
                               *primes.metadata(), ppc::PlanPollOptions{},
                               &ptasks, &error)) {
      std::fprintf(stderr, "PlanScanOnServer primes: %s\n", error.c_str());
      return 1;
    }
    std::vector<std::string> prime_paths;
    for (const auto& task : ptasks)
      prime_paths.push_back(task->data_file()->file_path);

    duckdb::DBConfig sconfig;
    sconfig.SetOptionByName("threads", duckdb::Value::BIGINT(opt.threads));
    sconfig.SetOptionByName("temp_directory",
                            duckdb::Value(opt.spill_dir + "/duckdb_tmp"));
    duckdb::DuckDB sdb(nullptr, &sconfig);
    duckdb::Connection scon(sdb);

    const std::string b = std::to_string(opt.bound);
    const std::string ssql =
        "SELECT p, m_k, n_k, q_k, false AS is_root FROM read_parquet(" +
        QuoteList(paths) + ") WHERE p <= " + b +
        " UNION ALL SELECT p, 0, 0, 0, true AS is_root FROM read_parquet(" +
        QuoteList(prime_paths) + ") WHERE k = 0 AND p <= " + b +
        " ORDER BY p";

    t0 = std::chrono::steady_clock::now();
    SliceSweeper sweeper(opt.spill_dir, opt.slice_width);
    auto sres = scon.SendQuery(ssql);
    if (sres->HasError()) {
      std::fprintf(stderr, "duckdb stream: %s\n", sres->GetError().c_str());
      return 1;
    }
    int64_t cur_p = -1;
    bool cur_root = false;
    std::vector<Edge> cur_edges;
    int64_t edge_count = 0;
    auto emit = [&](std::string* err) -> bool {
      if (cur_p < 0) return true;
      return sweeper.AddNode(cur_p, cur_root, cur_edges, err);
    };
    while (auto chunk = sres->Fetch()) {
      const duckdb::idx_t n = chunk->size();
      if (n == 0) break;
      auto* pv = duckdb::FlatVector::GetData<int64_t>(chunk->data[0]);
      auto* mv = duckdb::FlatVector::GetData<int32_t>(chunk->data[1]);
      auto* nv = duckdb::FlatVector::GetData<int32_t>(chunk->data[2]);
      auto* qv = duckdb::FlatVector::GetData<int64_t>(chunk->data[3]);
      auto* rv = duckdb::FlatVector::GetData<bool>(chunk->data[4]);
      for (duckdb::idx_t i = 0; i < n; ++i) {
        if (pv[i] != cur_p) {
          if (!emit(&error)) {
            std::fprintf(stderr, "sweep: %s\n", error.c_str());
            return 1;
          }
          cur_p = pv[i];
          cur_root = false;
          cur_edges.clear();
        }
        if (rv[i]) {
          cur_root = true;
        } else {
          cur_edges.push_back({pv[i], qv[i], mv[i], nv[i]});
          ++edge_count;
        }
      }
    }
    if (!emit(&error) || !sweeper.Finish(&error)) {
      std::fprintf(stderr, "sweep: %s\n", error.c_str());
      return 1;
    }
    const SweepResult& sw = sweeper.result();
    double t_sweep = Seconds(t0);
    std::printf("[wiring] part_files=%zu prime_files=%zu plan=%.2fs edges=%" PRId64
                " nodes=%" PRId64 " stream\n",
                paths.size(), prime_paths.size(), t_plan, edge_count, sw.nodes);

    if (opt.sweep_only) {
      std::printf(
          "[sweep] slices=%" PRId64 " width=%" PRId64 " memo_entries=%" PRId64
          " words=%" PRId64 " segs=%" PRId64 " connections=%" PRId64
          " spill_bytes=%" PRId64 " sweep=%.2fs\n",
          sw.num_slices, opt.slice_width, sw.memo_entries, sw.words, sw.segs,
          sw.connections, sw.spill_bytes, t_sweep);
      return 0;
    }
    return PublishFromSpill(opt, ns, sw.num_slices, sw.has_children,
                            sw.last_ref_slice, t_sweep);
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

  if (opt.cone_probe > 0) {
    std::vector<int64_t> sinks;
    for (int64_t v : nodes)
      if (!has_children.count(v)) sinks.push_back(v);
    const int K = opt.cone_probe;
    const size_t per = (sinks.size() + K - 1) / K;
    std::printf("[cone] nodes=%zu sinks=%zu batches=%d (~%zu sinks/batch)\n",
                nodes.size(), sinks.size(), K, per);
    for (int b = 0; b < K; ++b) {
      std::unordered_set<int64_t> cone;
      std::vector<int64_t> stack;
      const size_t lo = static_cast<size_t>(b) * per;
      const size_t hi = std::min(lo + per, sinks.size());
      if (lo >= hi) break;
      for (size_t i = lo; i < hi; ++i)
        if (cone.insert(sinks[i]).second) stack.push_back(sinks[i]);
      while (!stack.empty()) {
        int64_t v = stack.back();
        stack.pop_back();
        auto it = parents.find(v);
        if (it == parents.end()) continue;
        for (const auto& e : it->second)
          if (cone.insert(e.q).second) stack.push_back(e.q);
      }
      std::printf("[cone] batch %d sinks[%zu,%zu) cone_nodes=%zu (%.1f%%)\n", b,
                  lo, hi, cone.size(), 100.0 * cone.size() / nodes.size());
    }
    return 0;
  }

  if (!opt.spill_dir.empty()) {
    t0 = std::chrono::steady_clock::now();
    SweepResult sw;
    if (!SweepAndSpill(nodes, parents, opt.slice_width, opt.spill_dir, &sw,
                       &error)) {
      std::fprintf(stderr, "sweep: %s\n", error.c_str());
      return 1;
    }
    const int64_t num_slices = sw.num_slices;
    double t_sweep = Seconds(t0);
    t0 = std::chrono::steady_clock::now();

    if (opt.sweep_only) {
      std::printf(
          "[sweep] slices=%" PRId64 " width=%" PRId64 " memo_entries=%" PRId64
          " words=%" PRId64 " segs=%" PRId64 " connections=%" PRId64
          " spill_bytes=%" PRId64 " sweep=%.2fs\n",
          num_slices, opt.slice_width, sw.memo_entries, sw.words, sw.segs,
          sw.connections, sw.spill_bytes, t_sweep);
      return 0;
    }

    return PublishFromSpill(opt, ns, num_slices, has_children, sw.last_ref_slice,
                            t_sweep);
  }

  if (opt.slices >= 1) {
    t0 = std::chrono::steady_clock::now();
    ParResult sr = RunSliced(nodes, parents, has_children, opt.slices, opt.bound);
    std::printf(
        "[collapse] node_word_pairs=%" PRId64 " distinct_words=%" PRId64
        " max_degree=%" PRId64 " dp=%.2fs slices=%d\n",
        sr.node_word_pairs, sr.distinct_words, sr.max_degree, Seconds(t0),
        opt.slices);
    return 0;
  }

  if (opt.workers > 1) {
    t0 = std::chrono::steady_clock::now();
    ParResult pr =
        RunSlicedParallel(nodes, parents, has_children, 1000000, opt.workers);
    std::printf(
        "[collapse] node_word_pairs=%" PRId64 " distinct_words=%" PRId64
        " max_degree=%" PRId64 " dp=%.2fs workers=%d sliced\n",
        pr.node_word_pairs, pr.distinct_words, pr.max_degree, Seconds(t0),
        opt.workers);
    return 0;
  }

  t0 = std::chrono::steady_clock::now();
  WordStore store;
  const int32_t seed = store.seed;
  std::unordered_map<int64_t, std::vector<int32_t>> memo;
  std::priority_queue<std::pair<int64_t, int64_t>,
                      std::vector<std::pair<int64_t, int64_t>>,
                      std::greater<>>
      live;
  int64_t live_entries = 0;
  int64_t peak_entries = 0;

  std::unordered_map<int32_t, int64_t> sink_out;
  auto sink_id = [&](int32_t w) -> int64_t {
    auto f = sink_out.find(w);
    if (f != sink_out.end()) return f->second;
    int64_t id = static_cast<int64_t>(sink_out.size());
    sink_out.emplace(w, id);
    return id;
  };
  int64_t maximal_classes = 0;
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

  size_t sweep_idx = 0;
  size_t sweep_step = std::max<size_t>(1, nodes.size() / 20);
  std::vector<std::pair<int64_t, int64_t>> profile;

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
    if (sweep_idx++ % sweep_step == 0) profile.emplace_back(v, live_entries);

    std::unordered_set<int32_t> res;
    auto it = parents.find(v);
    if (it == parents.end()) {
      res.insert(seed);
    } else {
      for (const auto& e : it->second) {
        const int64_t c = int64_t{1} << e.m;
        auto pit = memo.find(e.q);
        if (pit == memo.end()) continue;
        for (int32_t w : pit->second) res.insert(store.Push(w, e.n, c));
      }
    }

    if (has_children.count(v)) {
      live_entries += static_cast<int64_t>(res.size());
      peak_entries = std::max(peak_entries, live_entries);
      memo[v] = std::vector<int32_t>(res.begin(), res.end());
      live.push({last_child[v], v});
    } else {
      for (int32_t w : res) {
        if (w == seed) continue;
        ++maximal_classes;
        max_degree = std::max(max_degree, store.Degree(w));
        int64_t oid = sink_id(w);
        if (ncw && !ncw->Add(v, oid, &error)) {
          std::fprintf(stderr, "node_classes write: %s\n", error.c_str());
          return 1;
        }
      }
    }
  }
  double t_dp = Seconds(t0);

  std::printf(
      "[collapse] node_word_pairs=%" PRId64 " distinct_words=%zu "
      "max_degree=%" PRId64 " dp=%.2fs peak_live_entries=%" PRId64 "\n",
      maximal_classes, sink_out.size(), max_degree, t_dp, peak_entries);
  for (const auto& [pos, live] : profile)
    std::printf("[profile] p<=%" PRId64 " live_entries=%" PRId64 "\n", pos,
                live);

  if (!opt.materialize) return 0;

  auto t_pub = std::chrono::steady_clock::now();
  std::vector<int32_t> out_word(sink_out.size());
  for (const auto& [w, id] : sink_out) out_word[id] = w;

  GiNaC::symbol x("x");
  ppq::MaterializeColumn cw_id{"word_id", ppq::ColumnType::kLong, {}, {}, false};
  ppq::MaterializeColumn cw_deg{"degree", ppq::ColumnType::kLong, {}, {}, false};
  ppq::MaterializeColumn cw_sk{"skeleton", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_tr{"translations", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_he{"hermite", ppq::ColumnType::kString, {}, {}, true};
  ppq::MaterializeColumn cw_sym{"symbolic", ppq::ColumnType::kString, {}, {}, true};
  int64_t a0;
  std::vector<std::pair<int32_t, int64_t>> segs;
  for (size_t i = 0; i < out_word.size(); ++i) {
    store.Reconstruct(out_word[i], &a0, &segs);
    Features f = WordFeatures(a0, segs, x);
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
  ppq::MaterializeOptions cw_opts;
  cw_opts.sort_keys = {"word_id"};
  cw_opts.target_file_bytes = int64_t{1} << 30;
  if (!ppq::MaterializeColumns(catalog, ns, opt.warehouse, "chain_words", cw,
                               cw_opts, &meta, &error)) {
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
      out_word.size(), ncw->total, files.size(), Seconds(t_pub));
  return 0;
}
