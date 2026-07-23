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
  int workers = 1;
  int slices = 0;
  int cone_probe = 0;
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

struct SegKey {
  int32_t parent;
  int32_t n;
  int64_t c;
  bool operator==(const SegKey& o) const {
    return parent == o.parent && n == o.n && c == o.c;
  }
};
struct SegKeyHash {
  size_t operator()(const SegKey& k) const {
    size_t h = std::hash<int32_t>{}(k.parent);
    h ^= std::hash<int32_t>{}(k.n) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(k.c) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};
struct WordKey {
  int64_t a0;
  int32_t tail;
  bool operator==(const WordKey& o) const {
    return a0 == o.a0 && tail == o.tail;
  }
};
struct WordKeyHash {
  size_t operator()(const WordKey& k) const {
    size_t h = std::hash<int64_t>{}(k.a0);
    h ^= std::hash<int32_t>{}(k.tail) + 0x9e3779b97f4a7c15ULL + (h << 6) +
         (h >> 2);
    return h;
  }
};

// Trie-DAG of composition words. A word is (a0, tail), tail indexing a chain of
// segments (n_i, C_i) via parent links (-1 = empty). Segments and words are
// interned, so every shared prefix is shared. Push composes: n=1 folds a
// translation into the tail (or a0), n>=2 appends a segment. The trie is the
// coproduct; degree is the product of the tail's exponents.
struct WordStore {
  std::vector<int32_t> seg_parent;
  std::vector<int32_t> seg_n;
  std::vector<int64_t> seg_c;
  std::vector<int64_t> seg_deg;
  std::unordered_map<SegKey, int32_t, SegKeyHash> seg_ix;

  std::vector<int64_t> word_a0;
  std::vector<int32_t> word_tail;
  std::unordered_map<WordKey, int32_t, WordKeyHash> word_ix;
  int32_t seed = 0;

  WordStore() { seed = InternWord(0, -1); }

  int32_t InternSeg(int32_t parent, int32_t n, int64_t c) {
    SegKey k{parent, n, c};
    auto it = seg_ix.find(k);
    if (it != seg_ix.end()) return it->second;
    int32_t id = static_cast<int32_t>(seg_parent.size());
    seg_parent.push_back(parent);
    seg_n.push_back(n);
    seg_c.push_back(c);
    seg_deg.push_back((parent < 0 ? 1 : seg_deg[parent]) * n);
    seg_ix.emplace(k, id);
    return id;
  }
  int32_t InternWord(int64_t a0, int32_t tail) {
    WordKey k{a0, tail};
    auto it = word_ix.find(k);
    if (it != word_ix.end()) return it->second;
    int32_t id = static_cast<int32_t>(word_a0.size());
    word_a0.push_back(a0);
    word_tail.push_back(tail);
    word_ix.emplace(k, id);
    return id;
  }
  int32_t Push(int32_t w, int32_t n, int64_t c) {
    int64_t a0 = word_a0[w];
    int32_t tail = word_tail[w];
    if (n == 1) {
      if (tail < 0) return InternWord(a0 + c, -1);
      return InternWord(a0,
                        InternSeg(seg_parent[tail], seg_n[tail], seg_c[tail] + c));
    }
    return InternWord(a0, InternSeg(tail, n, c));
  }
  int64_t Degree(int32_t w) const {
    int32_t tail = word_tail[w];
    return tail < 0 ? 1 : seg_deg[tail];
  }
  void Reconstruct(int32_t w, int64_t* a0,
                   std::vector<std::pair<int32_t, int64_t>>* segs) const {
    *a0 = word_a0[w];
    segs->clear();
    for (int32_t s = word_tail[w]; s >= 0; s = seg_parent[s])
      segs->emplace_back(seg_n[s], seg_c[s]);
    std::reverse(segs->begin(), segs->end());
  }
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

Features WordFeatures(int64_t a0,
                      const std::vector<std::pair<int32_t, int64_t>>& segs,
                      const GiNaC::symbol& x) {
  GiNaC::ex p = x + GiNaC::numeric(static_cast<long>(a0));
  std::vector<int64_t> sk;
  std::vector<int64_t> tr{a0};
  int64_t degree = 1;
  for (const auto& [n, c] : segs) {
    p = GiNaC::pow(p, n) + GiNaC::numeric(static_cast<long>(c));
    sk.push_back(n);
    tr.push_back(c);
    degree *= n;
  }
  auto hd = ToHermite(p, x);
  Features f;
  f.degree = degree;
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

// Concurrent trie store: sharded by key so interning does not serialize. A gid
// encodes (shard << 23 | local); each shard owns its vectors + map + mutex. The
// word carries its own degree so Degree/Push never chase a cross-shard seg for
// it; only the n=1 fold reads the tail seg's fields.
struct ConcStore {
  static constexpr int SH = 256;
  struct SegShard {
    std::mutex mu;
    std::unordered_map<SegKey, int32_t, SegKeyHash> ix;
    std::vector<int32_t> parent, n;
    std::vector<int64_t> c, deg;
  };
  struct WordShard {
    std::mutex mu;
    std::unordered_map<WordKey, int32_t, WordKeyHash> ix;
    std::vector<int64_t> a0, deg;
    std::vector<int32_t> tail;
  };
  SegShard sseg[SH];
  WordShard sword[SH];
  int32_t seed;

  ConcStore() { seed = InternWord(0, -1, 1); }

  int32_t InternSeg(int32_t parent, int32_t n, int64_t c, int64_t deg) {
    SegKey k{parent, n, c};
    int s = SegKeyHash{}(k) & (SH - 1);
    auto& sh = sseg[s];
    std::lock_guard<std::mutex> g(sh.mu);
    auto it = sh.ix.find(k);
    if (it != sh.ix.end()) return it->second;
    int32_t local = static_cast<int32_t>(sh.parent.size());
    sh.parent.push_back(parent);
    sh.n.push_back(n);
    sh.c.push_back(c);
    sh.deg.push_back(deg);
    int32_t gid = (s << 23) | local;
    sh.ix.emplace(k, gid);
    return gid;
  }
  void ReadSeg(int32_t gid, int32_t* parent, int32_t* n, int64_t* c) {
    auto& sh = sseg[gid >> 23];
    int32_t local = gid & 0x7FFFFF;
    std::lock_guard<std::mutex> g(sh.mu);
    *parent = sh.parent[local];
    *n = sh.n[local];
    *c = sh.c[local];
  }
  int32_t InternWord(int64_t a0, int32_t tail, int64_t deg) {
    WordKey k{a0, tail};
    int s = WordKeyHash{}(k) & (SH - 1);
    auto& sh = sword[s];
    std::lock_guard<std::mutex> g(sh.mu);
    auto it = sh.ix.find(k);
    if (it != sh.ix.end()) return it->second;
    int32_t local = static_cast<int32_t>(sh.a0.size());
    sh.a0.push_back(a0);
    sh.tail.push_back(tail);
    sh.deg.push_back(deg);
    int32_t gid = (s << 23) | local;
    sh.ix.emplace(k, gid);
    return gid;
  }
  void ReadWord(int32_t gid, int64_t* a0, int32_t* tail, int64_t* deg) {
    auto& sh = sword[gid >> 23];
    int32_t local = gid & 0x7FFFFF;
    std::lock_guard<std::mutex> g(sh.mu);
    *a0 = sh.a0[local];
    *tail = sh.tail[local];
    *deg = sh.deg[local];
  }
  int32_t Push(int32_t w, int32_t n, int64_t c) {
    int64_t a0, deg;
    int32_t tail;
    ReadWord(w, &a0, &tail, &deg);
    if (n == 1) {
      if (tail < 0) return InternWord(a0 + c, -1, deg);
      int32_t tp, tn;
      int64_t tc;
      ReadSeg(tail, &tp, &tn, &tc);
      return InternWord(a0, InternSeg(tp, tn, tc + c, deg), deg);
    }
    return InternWord(a0, InternSeg(tail, n, c, deg * n), deg * n);
  }
  int64_t Degree(int32_t w) {
    int64_t a0, deg;
    int32_t tail;
    ReadWord(w, &a0, &tail, &deg);
    return deg;
  }
};

struct ParResult {
  int64_t node_word_pairs = 0;
  int64_t distinct_words = 0;
  int64_t max_degree = 0;
};

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

// A partial chain: `word` is the segments accumulated within the current slice,
// `conn` is the boundary node it connects to below (-1 = complete, root is a k=0
// prime derivable as W^-1(p)). Reassembly composes `word` onto each of conn's
// chains. This is how a slice stays self-contained: a parent below the range is
// recorded as a connection, never traced.
struct MemoEntry {
  int32_t word;
  int64_t conn;
  bool operator==(const MemoEntry& o) const {
    return word == o.word && conn == o.conn;
  }
};
struct MemoEntryHash {
  size_t operator()(const MemoEntry& e) const {
    size_t h = std::hash<int32_t>{}(e.word);
    h ^= std::hash<int64_t>{}(e.conn) + 0x9e3779b97f4a7c15ULL + (h << 6) +
         (h >> 2);
    return h;
  }
};

// Compose the slice-local `upper` word onto a `base` chain by replaying upper's
// push sequence (its a0 as an n=1 fold, then each segment) onto base. This is
// exactly the deconcatenation coproduct run in reverse: Push(base, ...) built up
// from upper's (a0, segs).
int32_t Compose(WordStore& s, int32_t base, int32_t upper) {
  int64_t a0;
  std::vector<std::pair<int32_t, int64_t>> segs;
  s.Reconstruct(upper, &a0, &segs);
  int32_t t = base;
  if (a0 != 0) t = s.Push(t, 1, a0);
  for (const auto& [n, c] : segs) t = s.Push(t, n, c);
  return t;
}

void Expand(WordStore& s,
            const std::unordered_map<int64_t, std::vector<MemoEntry>>& memo,
            int32_t word, int64_t conn, std::unordered_set<int32_t>& out) {
  if (conn < 0) {
    out.insert(word);
    return;
  }
  auto it = memo.find(conn);
  if (it == memo.end()) return;
  for (const auto& e : it->second)
    Expand(s, memo, Compose(s, e.word, word), e.conn, out);
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
    } else if (a == "--cone-probe") {
      out->cone_probe = std::atoi(need("--cone-probe"));
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
    ParResult pr = RunParallel(nodes, parents, has_children, opt.workers);
    std::printf(
        "[collapse] node_word_pairs=%" PRId64 " distinct_words=%" PRId64
        " max_degree=%" PRId64 " dp=%.2fs workers=%d\n",
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
  ppq::MaterializeColumn cw_id{"word_id", ppq::ColumnType::kLong, {}, {}, true};
  ppq::MaterializeColumn cw_deg{"degree", ppq::ColumnType::kLong, {}, {}, true};
  ppq::MaterializeColumn cw_sk{"skeleton", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_tr{"translations", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_he{"hermite", ppq::ColumnType::kString, {}, {}, false};
  ppq::MaterializeColumn cw_sym{"symbolic", ppq::ColumnType::kString, {}, {}, false};
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
      out_word.size(), ncw->total, files.size(), Seconds(t_pub));
  return 0;
}
