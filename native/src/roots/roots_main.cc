#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <algorithm>
#include <memory>
#include <vector>

#include <arrow/record_batch.h>

#include "primeparts/client/session.h"
#include "primeparts/config.h"
#include "primeparts/parts_expand.h"
#include "primeparts/scan/column_binder.h"
#include "primeparts/scan/scan_plan.h"

#include "iceberg/expression/expression.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"

namespace {

namespace client = primeparts::client;
namespace scan = primeparts::scan;

struct Options {
  std::string config_path;
  int64_t p_hi = 0;
  int64_t p_lo = 0;
  std::string gaps_path;
  int64_t report_every = 50000000;
  int scan_threads = 8;
  double max_gb = 20.0;
  std::string exclude_path;
  std::string cand_path;
  bool light = false;
  std::string src_path;
  std::string emit_path;
};

struct Sketch {
  static constexpr int kBits = 8;
  static constexpr int kRegs = 1 << kBits;
  uint8_t reg[kRegs];

  void Clear() { std::memset(reg, 0, sizeof(reg)); }

  void Add(uint64_t x) {
    x *= 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 32;
    const uint32_t i = static_cast<uint32_t>(x >> (64 - kBits));
    const uint64_t w = x << kBits;
    const uint8_t rank =
        static_cast<uint8_t>(w == 0 ? 64 - kBits + 1 : __builtin_clzll(w) + 1);
    if (rank > reg[i]) reg[i] = rank;
  }

  void Merge(const Sketch& other) {
    for (int i = 0; i < kRegs; ++i) {
      if (other.reg[i] > reg[i]) reg[i] = other.reg[i];
    }
  }

  double Estimate() const {
    double sum = 0;
    int zeros = 0;
    for (int i = 0; i < kRegs; ++i) {
      sum += 1.0 / static_cast<double>(UINT64_C(1) << reg[i]);
      if (reg[i] == 0) ++zeros;
    }
    const double m = kRegs;
    double est = 0.7213 / (1.0 + 1.079 / m) * m * m / sum;
    if (est <= 2.5 * m && zeros > 0) est = m * std::log(m / zeros);
    return est;
  }
};

struct Interner {
  std::unordered_map<std::string, uint32_t> by_key;
  std::vector<uint32_t> singleton_root;
  uint64_t merges = 0;

  uint32_t Singleton(int64_t root) {
    const uint32_t id = static_cast<uint32_t>(singleton_root.size());
    singleton_root.push_back(static_cast<uint32_t>(root & 0xffffffff));
    return id;
  }

  uint32_t Union(std::vector<uint32_t>* ids) {
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
    if (ids->size() == 1) return (*ids)[0];
    std::string key(reinterpret_cast<const char*>(ids->data()),
                    ids->size() * sizeof(uint32_t));
    const auto at = by_key.find(key);
    if (at != by_key.end()) return at->second;
    const uint32_t id =
        static_cast<uint32_t>(singleton_root.size() + by_key.size());
    by_key.emplace(std::move(key), id);
    ++merges;
    return id;
  }

  std::size_t Count() const { return singleton_root.size() + by_key.size(); }
};

struct Cursor {
  client::TableHandle handle;
  std::unique_ptr<client::ScanStream> stream;
  std::shared_ptr<arrow::RecordBatch> batch;
  std::vector<const int64_t*> cols;
  std::vector<const int32_t*> cols32;
  std::vector<std::string> names;
  int64_t at = 0;
  bool done = false;

  bool Open(client::Session& session, const std::string& table,
            std::vector<std::string> select,
            const std::shared_ptr<iceberg::Expression>& filter,
            std::string* error) {
    names = select;
    if (!session.LoadTable(table, &handle, error)) return false;
    scan::ScanPlanRequest req;
    req.select = std::move(select);
    req.filter = filter;
    stream = session.Scan(handle, req, error);
    return stream != nullptr && Advance(error);
  }

  bool Advance(std::string* error) {
    while (!done && (batch == nullptr || at >= batch->num_rows())) {
      error->clear();
      if (!stream->Next(&batch, error)) {
        if (!error->empty()) return false;
        done = true;
        return true;
      }
      if (batch == nullptr) {
        done = true;
        return true;
      }
      at = 0;
      cols.assign(names.size(), nullptr);
      cols32.assign(names.size(), nullptr);
      for (std::size_t i = 0; i < names.size(); ++i) {
        std::string ignored;
        cols[i] = scan::BindInt64(*batch, names[i], &ignored);
        if (cols[i] != nullptr) continue;
        cols32[i] = scan::BindInt32(*batch, names[i], error);
        if (cols32[i] == nullptr) return false;
      }
    }
    return true;
  }

  bool Done() const { return done; }
  int64_t P() const { return Col(0); }
  int64_t Col(std::size_t i) const {
    return cols[i] != nullptr ? cols[i][at] : cols32[i][at];
  }
};

inline uint32_t RankIn(const int64_t* at, std::size_t n, int64_t q,
                       bool* found) {
  std::size_t lo = 0, hi = n;
  if (hi == 0 || q < at[0] || q > at[hi - 1]) { *found = false; return 0; }
  while (lo < hi) {
    const int64_t a = at[lo];
    const int64_t b = at[hi - 1];
    std::size_t mid;
    if (b > a && hi - lo > 8) {
      const double f = static_cast<double>(q - a) / static_cast<double>(b - a);
      mid = lo + static_cast<std::size_t>(f * static_cast<double>(hi - 1 - lo));
      if (mid < lo) mid = lo;
      if (mid >= hi) mid = hi - 1;
    } else {
      mid = lo + (hi - lo) / 2;
    }
    if (at[mid] == q) { *found = true; return static_cast<uint32_t>(mid); }
    if (at[mid] < q) lo = mid + 1; else hi = mid;
  }
  *found = false;
  return 0;
}

std::size_t Rss() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (f == nullptr) return 0;
  char line[256];
  long long kb = 0;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::sscanf(line, "RssAnon: %lld kB", &kb) == 1) break;
  }
  std::fclose(f);
  return static_cast<std::size_t>(kb) * 1024;
}

double Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::string Gb(std::size_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1e9);
  return buf;
}

int Run(const Options& opt) {
  primeparts::config::Conf conf;
  std::string error;
  if (!primeparts::config::Load(opt.config_path, &conf, &error)) {
    std::fprintf(stderr, "config: %s\n", error.c_str());
    return 1;
  }

  client::SessionOptions so;
  so.rest_uri = conf.core.rest_uri;
  so.warehouse = conf.core.warehouse;
  so.ns = conf.core.ns_name;
  so.scan_threads = 1;
  (void)opt.scan_threads;
  auto session = client::Session::Open(so, &error);
  if (!session) {
    std::fprintf(stderr, "session: %s\n", error.c_str());
    return 1;
  }

  std::shared_ptr<iceberg::Expression> filter;
  if (opt.p_hi > 0) {
    filter = iceberg::Expressions::LessThanOrEqual(
        "p", iceberg::Literal::Long(opt.p_hi));
  }
  if (opt.p_lo > 0) {
    auto lo = iceberg::Expressions::GreaterThanOrEqual(
        "p", iceberg::Literal::Long(opt.p_lo));
    filter = filter ? iceberg::Expressions::And(filter, lo) : lo;
  }

  Cursor primes, flat, twist;
  if (!primes.Open(*session, "primes", {"p", "k"}, filter, &error) ||
      !flat.Open(*session, "flat_parts", {"p", "hit_mask"}, filter, &error) ||
      !twist.Open(*session, "higher_parts", {"p", "q_k"}, filter, &error)) {
    std::fprintf(stderr, "open: %s\n", error.c_str());
    return 1;
  }
  std::fprintf(stderr, "planned primes=%lld flat=%lld twist=%lld\n",
               static_cast<long long>(primes.stream->planned_rows()),
               static_cast<long long>(flat.stream->planned_rows()),
               static_cast<long long>(twist.stream->planned_rows()));

  std::unordered_set<int64_t> blocked_roots;
  if (!opt.exclude_path.empty()) {
    std::FILE* f = std::fopen(opt.exclude_path.c_str(), "r");
    if (f == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", opt.exclude_path.c_str());
      return 1;
    }
    long long v = 0;
    while (std::fscanf(f, "%lld", &v) == 1) blocked_roots.insert(v);
    std::fclose(f);
    std::fprintf(stderr, "excluding %zu roots\n", blocked_roots.size());
  }

  std::vector<int64_t> cands;
  if (!opt.cand_path.empty()) {
    std::FILE* f = std::fopen(opt.cand_path.c_str(), "r");
    if (f == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", opt.cand_path.c_str());
      return 1;
    }
    long long v = 0;
    while (std::fscanf(f, "%lld", &v) == 1 && cands.size() < 64) {
      cands.push_back(v);
    }
    std::fclose(f);
    std::fprintf(stderr, "tracking %zu candidates\n", cands.size());
  }
  std::unordered_map<int64_t, int> cand_bit;
  for (std::size_t i = 0; i < cands.size(); ++i) {
    cand_bit.emplace(cands[i], static_cast<int>(i));
  }
  std::unordered_map<int64_t, int> src_bit;
  std::vector<int64_t> srcs;
  if (!opt.src_path.empty()) {
    std::FILE* f = std::fopen(opt.src_path.c_str(), "r");
    if (f == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", opt.src_path.c_str());
      return 1;
    }
    long long v = 0;
    while (std::fscanf(f, "%lld", &v) == 1 && srcs.size() < 64) srcs.push_back(v);
    std::fclose(f);
    for (std::size_t i = 0; i < srcs.size(); ++i) {
      src_bit.emplace(srcs[i], static_cast<int>(i));
    }
    std::fprintf(stderr, "seeding %zu sources\n", srcs.size());
  }
  std::vector<uint64_t> cov;
  std::vector<uint64_t> covf;
  std::vector<std::pair<int, int64_t>> cov_top;
  int cov_cut = 2;
  int64_t twist_only_nodes = 0;
  int64_t twist_only_bits = 0;
  int64_t total_bits = 0;
  std::vector<int64_t> twist_only_src(64, 0);

  std::vector<uint32_t> par_flat;
  std::vector<uint32_t> par_start;

  std::vector<bool> blocked;
  std::vector<Sketch> sketch;
  std::vector<int64_t> sketch_p;
  std::vector<double> chains;
  double best_chains = 0;
  int64_t best_chains_p = 0;
  std::vector<std::pair<double, int64_t>> chain_top;
  double chain_cut = 1.0;
  Interner interner;
  std::vector<uint32_t> parents;
  Sketch acc;
  double best_est = 0;
  int64_t best_p = 0;
  std::vector<std::pair<double, int64_t>> top;
  double est_cut = 1.0;
  int64_t planned = primes.stream->planned_rows();
  if (opt.p_hi > 2) {
    const double lp = std::log(static_cast<double>(opt.p_hi));
    const int64_t est =
        static_cast<int64_t>(1.15 * static_cast<double>(opt.p_hi) / lp);
    if (est < planned) planned = est;
  }
  if (planned > 0 && opt.emit_path.empty()) {
    const std::size_t n = static_cast<std::size_t>(planned);
    sketch_p.reserve(n);
    chains.reserve(n);
    blocked.reserve(n);
    if (!srcs.empty()) cov.reserve(n);
    if (!cands.empty()) par_start.reserve(n + 1);
    if (!opt.light) sketch.reserve(n);
    std::fprintf(stderr, "reserved for %lld primes, rss now %s\n",
                 static_cast<long long>(planned), Gb(Rss()).c_str());
  }

  constexpr int kMaxK = 24;
  std::vector<int64_t> k_all(kMaxK + 1, 0);
  std::vector<int64_t> k_merge(kMaxK + 1, 0);
  std::vector<double> k_chain_max(kMaxK + 1, 0);
  std::vector<double> k_chain_sum(kMaxK + 1, 0);
  int64_t merges = 0;
  std::size_t last_rss = 0;
  const uint16_t* gap_map = nullptr;
  std::vector<int64_t> ckpt;
  std::size_t rank_n = 0;
  int64_t first_prime = 0;
  if (!opt.emit_path.empty()) {
    const std::string base =
        opt.gaps_path.empty() ? opt.emit_path : opt.gaps_path;
    const std::string gv = base + ".g";
    const std::string cv = base + ".ck";
    if (!opt.gaps_path.empty()) {
      std::FILE* probe = std::fopen(gv.c_str(), "rb");
      if (probe == nullptr) {
        std::fprintf(stderr, "no gap file at %s\n", gv.c_str());
        return 1;
      }
      std::fseek(probe, 0, SEEK_END);
      const std::size_t gbytes = std::ftell(probe);
      std::fclose(probe);
      const int fd2 = open(gv.c_str(), O_RDONLY);
      void* m2 = mmap(nullptr, gbytes, PROT_READ, MAP_SHARED, fd2, 0);
      close(fd2);
      if (m2 == MAP_FAILED) { std::fprintf(stderr, "mmap gaps failed\n"); return 1; }
      madvise(m2, gbytes, MADV_RANDOM);
      gap_map = static_cast<const uint16_t*>(m2);
      rank_n = gbytes / sizeof(uint16_t) + 1;
      std::FILE* cr = std::fopen(cv.c_str(), "rb");
      if (cr == nullptr) { std::fprintf(stderr, "no checkpoints\n"); return 1; }
      std::fseek(cr, 0, SEEK_END);
      ckpt.resize(std::ftell(cr) / 8);
      std::rewind(cr);
      if (std::fread(ckpt.data(), sizeof(int64_t), ckpt.size(), cr) != ckpt.size()) {
        std::fprintf(stderr, "short checkpoints\n"); return 1;
      }
      std::fclose(cr);
      first_prime = ckpt.empty() ? 3 : ckpt[0];
      std::fprintf(stderr, "reusing gaps: %zu primes\n", rank_n);
    } else {
    std::FILE* gf = std::fopen(gv.c_str(), "wb");
    std::FILE* cf = std::fopen(cv.c_str(), "wb");
    if (gf == nullptr || cf == nullptr) {
      std::fprintf(stderr, "cannot write %s / %s\n", gv.c_str(), cv.c_str());
      return 1;
    }
    Cursor only;
    if (!only.Open(*session, "primes", {"p", "k"}, filter, &error)) {
      std::fprintf(stderr, "phase 1: %s\n", error.c_str());
      return 1;
    }
    std::vector<uint16_t> gbuf;
    gbuf.reserve(1 << 20);
    int64_t n = 0, prev = 0;
    const double tp = Now();
    while (!only.Done()) {
      const int64_t v = only.P();
      if (n == 0) {
        first_prime = v;
        std::fwrite(&v, sizeof(int64_t), 1, cf);
      } else {
        const int64_t d = (v - prev) / 2;
        if (d <= 0 || d > 65535) {
          std::fprintf(stderr, "gap %lld out of range at p=%lld\n",
                       static_cast<long long>(v - prev),
                       static_cast<long long>(v));
          return 1;
        }
        gbuf.push_back(static_cast<uint16_t>(d));
        if (n % 4096 == 0) std::fwrite(&v, sizeof(int64_t), 1, cf);
      }
      prev = v;
      ++n;
      if (gbuf.size() >= (1 << 20)) {
        std::fwrite(gbuf.data(), sizeof(uint16_t), gbuf.size(), gf);
        gbuf.clear();
      }
      ++only.at;
      if (!only.Advance(&error)) {
        std::fprintf(stderr, "phase 1 advance: %s\n", error.c_str());
        return 1;
      }
    }
    if (!gbuf.empty()) {
      std::fwrite(gbuf.data(), sizeof(uint16_t), gbuf.size(), gf);
    }
    std::fclose(gf);
    std::fclose(cf);
    std::fprintf(stderr, "phase 1: %lld primes, gaps to %s, %.0fs\n",
                 static_cast<long long>(n), gv.c_str(), Now() - tp);

    const int fd = open(gv.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "cannot mmap %s\n", gv.c_str()); return 1; }
    const std::size_t bytes = static_cast<std::size_t>(n - 1) * sizeof(uint16_t);
    void* m = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { std::fprintf(stderr, "mmap failed\n"); return 1; }
    madvise(m, bytes, MADV_WILLNEED);
    gap_map = static_cast<const uint16_t*>(m);
    rank_n = static_cast<std::size_t>(n);

    std::FILE* cr = std::fopen(cv.c_str(), "rb");
    std::fseek(cr, 0, SEEK_END);
    ckpt.resize(std::ftell(cr) / 8);
    std::rewind(cr);
    if (std::fread(ckpt.data(), sizeof(int64_t), ckpt.size(), cr) != ckpt.size()) {
      std::fprintf(stderr, "short checkpoint file\n");
      return 1;
    }
    std::fclose(cr);
    }
  }

  std::vector<std::size_t> mcur(64, 0);
  std::vector<int64_t> mval(64, 0);

  std::FILE* emit = nullptr;
  std::FILE* emit_p = nullptr;
  std::vector<uint32_t> emit_buf;
  if (!opt.emit_path.empty()) {
    emit = std::fopen(opt.emit_path.c_str(), "wb");
    if (emit == nullptr) {
      std::fprintf(stderr, "cannot write %s\n", opt.emit_path.c_str());
      return 1;
    }
    emit_buf.reserve(1 << 20);
    std::fprintf(stderr, "emitting edges to %s\n", opt.emit_path.c_str());
  }

  const bool emitting = emit != nullptr;
  int64_t rows = 0, roots = 0, missing = 0, orphan = 0, last_p = 0;
  const double t0 = Now();
  double next_report = opt.report_every;

  while (!primes.Done()) {
    const int64_t p = primes.P();
    const int64_t k = primes.Col(1);
    if (p < last_p) {
      std::fprintf(stderr, "primes not ascending (%lld after %lld)\n",
                   static_cast<long long>(p), static_cast<long long>(last_p));
      return 1;
    }
    last_p = p;

    acc.Clear();
    double ch = 0;
    uint64_t cm = 0;
    uint64_t cmf = 0;
    int par_pop = 0;
    parents.clear();
    bool block = blocked_roots.count(p) != 0;
    while (!flat.Done() && flat.P() < p) {
      ++flat.at;
      if (!flat.Advance(&error)) { std::fprintf(stderr, "flat: %s\n", error.c_str()); return 1; }
    }
    if (!flat.Done() && flat.P() == p) {
      primeparts::ForEachPart(p, static_cast<uint64_t>(flat.Col(1)),
                              [&](int32_t m, int64_t q) {
                                bool ok = false;
                                uint32_t ix = 0;
                                if (gap_map != nullptr) {
                                  std::size_t& c = mcur[m];
                                  int64_t& v = mval[m];
                                  if (v == 0) {
                                    std::size_t a = 0, b = ckpt.size();
                                    while (a + 1 < b) {
                                      const std::size_t mid = a + (b - a) / 2;
                                      if (ckpt[mid] <= q) a = mid; else b = mid;
                                    }
                                    c = a * 4096;
                                    v = ckpt[a];
                                  }
                                  while (c + 1 < rank_n && v < q) {
                                    v += 2 * static_cast<int64_t>(gap_map[c]);
                                    ++c;
                                  }
                                  ok = v == q;
                                  ix = static_cast<uint32_t>(c);
                                } else {
                                  ix = RankIn(sketch_p.data(), sketch_p.size(),
                                              q, &ok);
                                }
                                if (!ok) {
                                  ++missing;
                                  return;
                                }
                                parents.push_back(ix);
                                if (emitting) return;
                                if (!opt.light) acc.Merge(sketch[ix]);
                                ch += chains[ix];
                                if (!srcs.empty()) {
                                  cm |= cov[ix];
                                  cmf |= covf[ix];
                                  const int pp = __builtin_popcountll(cov[ix]);
                                  if (pp > par_pop) par_pop = pp;
                                }
                                if (blocked[ix]) block = true;
                              });
      ++flat.at;
      if (!flat.Advance(&error)) { std::fprintf(stderr, "flat: %s\n", error.c_str()); return 1; }
    }
    while (!twist.Done() && twist.P() < p) {
      ++twist.at;
      if (!twist.Advance(&error)) { std::fprintf(stderr, "twist: %s\n", error.c_str()); return 1; }
    }
    while (!twist.Done() && twist.P() == p) {
      bool ok = false;
      uint32_t ix = 0;
      if (gap_map != nullptr) {
        const int64_t q = twist.Col(1);
        std::size_t lo = 0, hi = ckpt.size();
        while (lo + 1 < hi) {
          const std::size_t mid = lo + (hi - lo) / 2;
          if (ckpt[mid] <= q) lo = mid; else hi = mid;
        }
        std::size_t c = lo * 4096;
        int64_t v = ckpt[lo];
        while (c + 1 < rank_n && v < q) {
          v += 2 * static_cast<int64_t>(gap_map[c]);
          ++c;
        }
        ok = v == q;
        ix = static_cast<uint32_t>(c);
      } else {
        ix = RankIn(sketch_p.data(), sketch_p.size(), twist.Col(1), &ok);
      }
      if (!ok) {
        ++missing;
      } else if (emitting) {
        parents.push_back(ix);
      } else {
        parents.push_back(ix);
        if (!opt.light) acc.Merge(sketch[ix]);
        ch += chains[ix];
        if (!srcs.empty()) {
          cm |= cov[ix];
          const int pp = __builtin_popcountll(cov[ix]);
          if (pp > par_pop) par_pop = pp;
        }
        if (blocked[ix]) block = true;
      }
      ++twist.at;
      if (!twist.Advance(&error)) { std::fprintf(stderr, "twist: %s\n", error.c_str()); return 1; }
    }

    if (parents.empty()) {
      acc.Clear();
      acc.Add(static_cast<uint64_t>(p));
      ch = 1;
      ++roots;
      if (k != 0) ++orphan;
    }
    if (!srcs.empty()) {
      const auto sb = src_bit.find(p);
      if (sb != src_bit.end()) {
        cm |= UINT64_C(1) << sb->second;
        cmf |= UINT64_C(1) << sb->second;
      }
      cov.push_back(cm);
      covf.push_back(cmf);
      total_bits += __builtin_popcountll(cm);
      const uint64_t only = cm & ~cmf;
      if (only != 0) {
        ++twist_only_nodes;
        twist_only_bits += __builtin_popcountll(only);
        for (int b = 0; b < 64; ++b) {
          if ((only >> b) & 1) ++twist_only_src[b];
        }
      }
      const int pc = __builtin_popcountll(cm);
      if (pc >= cov_cut) {
        cov_top.emplace_back(pc, p);
        if (cov_top.size() >= 4096) {
          std::sort(cov_top.begin(), cov_top.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
          cov_top.resize(256);
          cov_cut = cov_top.back().first;
        }
      }
    }
    if (emit != nullptr) {
      emit_buf.push_back(static_cast<uint32_t>(parents.size()));
      for (const uint32_t ix : parents) emit_buf.push_back(ix);
      if (emit_buf.size() >= (1 << 20)) {
        std::fwrite(emit_buf.data(), sizeof(uint32_t), emit_buf.size(), emit);
        emit_buf.clear();
      }
    }
    if (!emitting) {
      const int kb = k < 0 ? 0 : (k > kMaxK ? kMaxK : static_cast<int>(k));
      ++k_all[kb];
      k_chain_sum[kb] += ch;
      if (ch > k_chain_max[kb]) k_chain_max[kb] = ch;
      if (!srcs.empty() && __builtin_popcountll(cm) > par_pop &&
          __builtin_popcountll(cm) >= 2) {
        ++k_merge[kb];
        ++merges;
      }
    }
    if (!emitting) chains.push_back(ch);
    if (ch > best_chains) {
      best_chains = ch;
      best_chains_p = p;
    }
    if (ch > chain_cut) {
      chain_top.emplace_back(ch, p);
      if (chain_top.size() >= 4096) {
        std::sort(chain_top.begin(), chain_top.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        chain_top.resize(256);
        chain_cut = chain_top.back().first;
      }
    }
    if (!emitting) {
      if (!opt.light) sketch.push_back(acc);
      if (gap_map == nullptr) sketch_p.push_back(p);
      blocked.push_back(block);
    }
    if (!cands.empty()) {
      par_start.push_back(static_cast<uint32_t>(par_flat.size()));
      for (const uint32_t ix : parents) par_flat.push_back(ix);
    }
    const double est = block ? 0.0 : acc.Estimate();
    if (est > best_est) {
      best_est = est;
      best_p = p;
    }
    if (est > est_cut) {
      top.emplace_back(est, p);
      if (top.size() >= 4096) {
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        top.resize(256);
        est_cut = top.back().first;
      }
    }

    const std::size_t live = (rows & 0xffff) == 0 ? Rss() : last_rss;
    last_rss = live;
    if (static_cast<double>(live) > opt.max_gb * 1e9) {
      std::fprintf(stderr,
                   "STOP at the %.0f GB budget: p=%lld rows=%lld roots=%lld "
                   "sets=%zu sets/rows=%.4f  %.0fs\n",
                   opt.max_gb, static_cast<long long>(p),
                   static_cast<long long>(rows), static_cast<long long>(roots),
                   interner.Count(),
                   static_cast<double>(interner.Count()) / rows, Now() - t0);
      return 0;
    }
    if (++rows >= next_report) {
      next_report += opt.report_every;
      const std::size_t bytes = live;
      std::fprintf(stderr,
                   "p=%lld rows=%lld roots=%lld most-chains=%lld (%.3g) "
                   "missing=%lld ~%s %.0fs\n",
                   static_cast<long long>(p), static_cast<long long>(rows),
                   static_cast<long long>(roots),
                   static_cast<long long>(best_chains_p), best_chains,
                   static_cast<long long>(missing), Gb(bytes).c_str(),
                   Now() - t0);
    }
    ++primes.at;
    if (!primes.Advance(&error)) { std::fprintf(stderr, "primes: %s\n", error.c_str()); return 1; }
  }
  if (!error.empty()) {
    std::fprintf(stderr, "scan ended: %s\n", error.c_str());
    return 1;
  }
  if (!cands.empty()) {
    par_start.push_back(static_cast<uint32_t>(par_flat.size()));
    std::vector<uint64_t> mask(sketch_p.size(), 0);
    for (std::size_t i = 0; i < sketch_p.size(); ++i) {
      const auto at = cand_bit.find(sketch_p[i]);
      if (at != cand_bit.end()) mask[i] |= UINT64_C(1) << at->second;
    }
    for (std::size_t i = sketch_p.size(); i-- > 0;) {
      const uint64_t m = mask[i];
      if (m == 0) continue;
      for (uint32_t j = par_start[i]; j < par_start[i + 1]; ++j) {
        mask[par_flat[j]] |= m;
      }
    }
    std::unordered_map<uint64_t, int64_t> by_mask;
    std::vector<int64_t> per_cand(cands.size(), 0);
    int64_t untouched = 0;
    int64_t first_gap = 0;
    std::vector<int64_t> gaps;
    for (std::size_t i = 0; i < sketch_p.size(); ++i) {
      if (par_start[i] != par_start[i + 1]) continue;
      if (mask[i] == 0) {
        ++untouched;
        if (first_gap == 0) first_gap = sketch_p[i];
        if (gaps.size() < 20) gaps.push_back(sketch_p[i]);
        continue;
      }
      ++by_mask[mask[i]];
      for (std::size_t c = 0; c < cands.size(); ++c) {
        if ((mask[i] >> c) & 1) ++per_cand[c];
      }
    }
    std::fprintf(stderr,
                 "\nsmallest k=0 prime reached by no candidate: %lld\n",
                 static_cast<long long>(first_gap));
    std::fprintf(stderr, "first uncovered sources:");
    for (const int64_t g : gaps) {
      std::fprintf(stderr, " %lld", static_cast<long long>(g));
    }
    std::fprintf(stderr, "\n\nroots by candidate:\n");
    for (std::size_t c = 0; c < cands.size(); ++c) {
      std::fprintf(stderr, "  [%2zu] p=%-11lld roots=%lld\n", c,
                   static_cast<long long>(cands[c]),
                   static_cast<long long>(per_cand[c]));
    }
    std::vector<std::pair<int64_t, uint64_t>> cells;
    for (const auto& [m, n] : by_mask) cells.emplace_back(n, m);
    std::sort(cells.begin(), cells.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr,
                 "\n%zu distinct candidate-subsets over the roots; "
                 "%lld roots reach no candidate\n",
                 cells.size(), static_cast<long long>(untouched));
    for (std::size_t i = 0; i < cells.size() && i < 16; ++i) {
      std::fprintf(stderr, "  %10lld roots feed %d candidates  mask=%llx\n",
                   static_cast<long long>(cells[i].first),
                   __builtin_popcountll(cells[i].second),
                   static_cast<unsigned long long>(cells[i].second));
    }
  }

  if (!srcs.empty()) {
    std::sort(cov_top.begin(), cov_top.end(),
              [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second < b.second;
              });
    std::fprintf(stderr, "\nnodes covering the most seeded sources:\n");
    for (std::size_t i = 0; i < cov_top.size() && i < 20; ++i) {
      std::fprintf(stderr, "  p=%-13lld covers %d of %zu\n",
                   static_cast<long long>(cov_top[i].second), cov_top[i].first,
                   srcs.size());
    }
    if (cov_top.empty()) {
      std::fprintf(stderr, "  none cover more than one\n");
    }
    std::fprintf(stderr,
                 "\n(node, source) incidences: %lld\n"
                 "reached only through a twist: %lld (%.6f%%) at %lld nodes\n",
                 static_cast<long long>(total_bits),
                 static_cast<long long>(twist_only_bits),
                 100.0 * static_cast<double>(twist_only_bits) /
                     static_cast<double>(total_bits == 0 ? 1 : total_bits),
                 static_cast<long long>(twist_only_nodes));
    for (std::size_t i = 0; i < srcs.size(); ++i) {
      if (twist_only_src[i] == 0) continue;
      std::fprintf(stderr, "  source %-10lld twist-only at %lld nodes\n",
                   static_cast<long long>(srcs[i]),
                   static_cast<long long>(twist_only_src[i]));
    }
  }

  if (emit != nullptr) {
    if (!emit_buf.empty()) {
      std::fwrite(emit_buf.data(), sizeof(uint32_t), emit_buf.size(), emit);
    }
    std::fclose(emit);
    std::fprintf(stderr, "edges written\n");
  }

  std::fprintf(stderr, "\n k     primes        share    mean chains   max chains");
  if (!srcs.empty()) std::fprintf(stderr, "   first merges");
  std::fprintf(stderr, "\n");
  for (int i = 0; i <= kMaxK; ++i) {
    if (k_all[i] == 0) continue;
    std::fprintf(stderr, "%2d  %12lld  %7.4f%%  %11.3g  %11.3g", i,
                 static_cast<long long>(k_all[i]),
                 100.0 * static_cast<double>(k_all[i]) / rows,
                 k_chain_sum[i] / static_cast<double>(k_all[i]),
                 k_chain_max[i]);
    if (!srcs.empty()) {
      std::fprintf(stderr, "  %12lld", static_cast<long long>(k_merge[i]));
    }
    std::fprintf(stderr, "\n");
  }
  if (!srcs.empty()) {
    std::fprintf(stderr, "total first merges: %lld\n",
                 static_cast<long long>(merges));
  }

  std::sort(chain_top.begin(), chain_top.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::fprintf(stderr, "\nmost chains:\n");
  for (std::size_t i = 0; i < chain_top.size() && i < 20; ++i) {
    std::fprintf(stderr, "  p=%-12lld chains=%.6g\n",
                 static_cast<long long>(chain_top[i].second),
                 chain_top[i].first);
  }

  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::fprintf(stderr, "done: rows=%lld roots=%lld missing=%lld %.0fs\n",
               static_cast<long long>(rows), static_cast<long long>(roots),
               static_cast<long long>(missing), Now() - t0);
  for (std::size_t i = 0; i < top.size() && i < 64; ++i) {
    std::fprintf(stderr, "  candidate p=%lld  ~%.0f roots  %.1f%% of X\n",
                 static_cast<long long>(top[i].second), top[i].first,
                 100.0 * top[i].first / roots);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  static const option kLong[] = {
      {"config", required_argument, nullptr, 'c'},
      {"p-hi", required_argument, nullptr, 'H'},
      {"p-lo", required_argument, nullptr, 'O'},
      {"gaps", required_argument, nullptr, 'G'},
      {"report-every", required_argument, nullptr, 'r'},
      {"threads", required_argument, nullptr, 't'},
      {"max-gb", required_argument, nullptr, 'g'},
      {"exclude", required_argument, nullptr, 'x'},
      {"candidates", required_argument, nullptr, 'C'},
      {"light", no_argument, nullptr, 'L'},
      {"sources", required_argument, nullptr, 'S'},
      {"emit-edges", required_argument, nullptr, 'E'},
      {nullptr, 0, nullptr, 0}};
  for (int c; (c = getopt_long(argc, argv, "c:H:O:r:t:g:x:C:LS:E:G:", kLong, nullptr)) != -1;) {
    switch (c) {
      case 'c': opt.config_path = optarg; break;
      case 'H': opt.p_hi = std::atoll(optarg); break;
      case 'O': opt.p_lo = std::atoll(optarg); break;
      case 'G': opt.gaps_path = optarg; break;
      case 'r': opt.report_every = std::atoll(optarg); break;
      case 't': opt.scan_threads = std::atoi(optarg); break;
      case 'g': opt.max_gb = std::atof(optarg); break;
      case 'x': opt.exclude_path = optarg; break;
      case 'C': opt.cand_path = optarg; break;
      case 'L': opt.light = true; break;
      case 'S': opt.src_path = optarg; break;
      case 'E': opt.emit_path = optarg; break;
      default:
        std::fprintf(stderr,
                     "usage: %s [-c config] [--p-hi N] [--report-every N] "
                     "[--threads N]\n",
                     argv[0]);
        return 2;
    }
  }
  return Run(opt);
}
