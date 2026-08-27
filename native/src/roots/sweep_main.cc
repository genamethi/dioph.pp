#include <getopt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <utility>
#include <vector>

namespace {

double Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::size_t Rss() {
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  long long total = 0, resident = 0;
  const int n = std::fscanf(f, "%lld %lld", &total, &resident);
  std::fclose(f);
  return n == 2 ? static_cast<std::size_t>(resident) * 4096 : 0;
}

std::vector<int64_t> g_ckpt;

struct Parts {
  std::vector<std::string> names;
  std::vector<uint64_t> start;  // first word index of each part
  std::size_t at = 0;
  std::FILE* f = nullptr;

  bool Open(const std::string& path) {
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".list") == 0) {
      std::FILE* l = std::fopen(path.c_str(), "r");
      if (l == nullptr) return false;
      char buf[512];
      while (std::fgets(buf, sizeof(buf), l) != nullptr) {
        std::string n(buf);
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
        if (!n.empty()) names.push_back(n);
      }
      std::fclose(l);
    } else {
      names.push_back(path);
    }
    uint64_t acc = 0;
    for (const std::string& n : names) {
      start.push_back(acc);
      std::FILE* t = std::fopen(n.c_str(), "rb");
      if (t == nullptr) return false;
      std::fseek(t, 0, SEEK_END);
      acc += static_cast<uint64_t>(std::ftell(t)) / 4;
      std::fclose(t);
    }
    start.push_back(acc);
    return Next();
  }

  bool Seek(uint64_t word) {
    std::size_t i = 0;
    while (i + 1 < start.size() && start[i + 1] <= word) ++i;
    if (i >= names.size()) return false;
    if (f != nullptr) std::fclose(f);
    f = std::fopen(names[i].c_str(), "rb");
    if (f == nullptr) return false;
    at = i + 1;
    std::fseek(f, static_cast<long>((word - start[i]) * 4), SEEK_SET);
    return true;
  }

  bool Next() {
    if (f != nullptr) std::fclose(f);
    f = nullptr;
    while (at < names.size()) {
      f = std::fopen(names[at++].c_str(), "rb");
      if (f != nullptr) return true;
    }
    return false;
  }

  std::size_t Read(uint32_t* dst, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
      if (f == nullptr) break;
      const std::size_t r = std::fread(dst + got, sizeof(uint32_t), n - got, f);
      got += r;
      if (got < n && !Next()) break;
    }
    return got;
  }

  void Rewind() {
    if (f != nullptr) std::fclose(f);
    f = nullptr;
    at = 0;
    Next();
  }
};

bool LoadCkpt(const std::string& base) {
  if (!g_ckpt.empty()) return true;
  std::FILE* f = std::fopen((base + ".ck").c_str(), "rb");
  if (f == nullptr) return false;
  std::fseek(f, 0, SEEK_END);
  g_ckpt.resize(std::ftell(f) / 8);
  std::rewind(f);
  const bool ok =
      std::fread(g_ckpt.data(), sizeof(int64_t), g_ckpt.size(), f) ==
      g_ckpt.size();
  std::fclose(f);
  return ok;
}

int64_t PrimeAt(const std::string& base, uint32_t rank) {
  if (!LoadCkpt(base)) return 0;
  const std::size_t blk = rank / 4096;
  if (blk >= g_ckpt.size()) return 0;
  int64_t v = g_ckpt[blk];
  const std::size_t from = blk * 4096;
  if (rank == from) return v;
  std::FILE* g = std::fopen((base + ".g").c_str(), "rb");
  if (g == nullptr) return 0;
  std::fseek(g, static_cast<long>(from) * 2, SEEK_SET);
  std::vector<uint16_t> buf(rank - from);
  if (std::fread(buf.data(), sizeof(uint16_t), buf.size(), g) != buf.size()) {
    std::fclose(g);
    return 0;
  }
  std::fclose(g);
  for (const uint16_t d : buf) v += 2 * static_cast<int64_t>(d);
  return v;
}

}  // namespace

int64_t RankOfPrime(const std::string& base, int64_t q) {
  if (!LoadCkpt(base)) return -1;
  std::size_t lo = 0, hi = g_ckpt.size();
  while (lo + 1 < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (g_ckpt[mid] <= q) lo = mid; else hi = mid;
  }
  int64_t v = g_ckpt[lo];
  if (v == q) return static_cast<int64_t>(lo) * 4096;
  std::FILE* g = std::fopen((base + ".g").c_str(), "rb");
  if (g == nullptr) return -1;
  std::fseek(g, static_cast<long>(lo) * 4096 * 2, SEEK_SET);
  std::vector<uint16_t> buf(8192);
  const std::size_t got =
      std::fread(buf.data(), sizeof(uint16_t), buf.size(), g);
  std::fclose(g);
  for (std::size_t i = 0; i < got; ++i) {
    v += 2 * static_cast<int64_t>(buf[i]);
    if (v == q) return static_cast<int64_t>(lo) * 4096 + i + 1;
    if (v > q) break;
  }
  return -1;
}

int main(int argc, char** argv) {
  std::string edges;
  std::string src_path;
  std::vector<int64_t> at_primes;
  int64_t seed = 0;
  bool do_index = false;
  int64_t report = 100000000;
  static const option kLong[] = {{"edges", required_argument, nullptr, 'e'},
                                 {"report-every", required_argument, nullptr, 'r'},
                                 {"sources", required_argument, nullptr, 'S'},
                                 {"at", required_argument, nullptr, 'A'},
                                 {"seed", required_argument, nullptr, 'D'},
                                 {"index", no_argument, nullptr, 'I'},
                                 {nullptr, 0, nullptr, 0}};
  for (int c; (c = getopt_long(argc, argv, "e:r:S:A:D:I", kLong, nullptr)) != -1;) {
    switch (c) {
      case 'e': edges = optarg; break;
      case 'r': report = std::atoll(optarg); break;
      case 'S': src_path = optarg; break;
      case 'A': at_primes.push_back(std::atoll(optarg)); break;
      case 'D': seed = std::atoll(optarg); break;
      case 'I': do_index = true; break;
      default: std::fprintf(stderr, "usage: %s --edges FILE\n", argv[0]); return 2;
    }
  }
  if (edges.empty()) {
    std::fprintf(stderr, "usage: %s --edges FILE\n", argv[0]);
    return 2;
  }

  Parts src;
  if (!src.Open(edges)) {
    std::fprintf(stderr, "cannot open %s\n", edges.c_str());
    return 1;
  }
  std::fprintf(stderr, "reading %zu part(s)\n", src.names.size());

  constexpr uint32_t kBlock = 4096;
  std::string base = edges;
  if (base.size() > 5 && base.compare(base.size() - 5, 5, ".list") == 0) {
    base = base.substr(0, base.size() - 5);
  }

  if (do_index) {
    std::FILE* ix = std::fopen((base + ".idx").c_str(), "wb");
    if (ix == nullptr) { std::fprintf(stderr, "cannot write idx\n"); return 1; }
    std::vector<uint32_t> b(1 << 20);
    std::size_t at = 0, have = 0;
    uint64_t words = 0;
    uint32_t r = 0;
    const auto fl = [&]() {
      if (at < have) return true;
      have = src.Read(b.data(), b.size());
      at = 0;
      return have > 0;
    };
    const double ti = Now();
    while (fl()) {
      if (r % kBlock == 0) std::fwrite(&words, sizeof(uint64_t), 1, ix);
      const uint32_t deg = b[at++];
      ++words;
      for (uint32_t i = 0; i < deg; ++i) { if (!fl()) break; ++at; ++words; }
      ++r;
    }
    std::fwrite(&words, sizeof(uint64_t), 1, ix);
    std::fclose(ix);
    src.Rewind();
    std::fprintf(stderr, "indexed %u records, %llu words, %.0fs\n", r,
                 (unsigned long long)words, Now() - ti);
    return 0;
  }

  if (seed != 0) {
    const int64_t sr = RankOfPrime(base, seed);
    if (sr < 0) { std::fprintf(stderr, "seed not found\n"); return 1; }
    std::FILE* ix = std::fopen((base + ".idx").c_str(), "rb");
    if (ix == nullptr) { std::fprintf(stderr, "run --index first\n"); return 1; }
    std::fseek(ix, 0, SEEK_END);
    const long nblk = std::ftell(ix) / 8 - 1;
    std::vector<uint64_t> off(nblk + 1);
    std::rewind(ix);
    if (std::fread(off.data(), sizeof(uint64_t), nblk + 1, ix) != (size_t)(nblk + 1)) {
      std::fprintf(stderr, "short idx\n"); return 1;
    }
    std::fclose(ix);

    const uint64_t total = static_cast<uint64_t>(nblk) * kBlock;
    std::vector<bool> mark(total + kBlock, false);
    mark[sr] = true;
    std::vector<uint32_t> blk;
    std::vector<uint32_t> starts;
    int64_t marked = 1;
    const double td = Now();
    for (long b = nblk - 1; b >= 0; --b) {
      const uint64_t w0 = off[b], w1 = off[b + 1];
      blk.resize(w1 - w0);
      if (!src.Seek(w0)) break;
      if (src.Read(blk.data(), blk.size()) != blk.size()) break;
      starts.clear();
      for (std::size_t i = 0; i < blk.size();) {
        starts.push_back(static_cast<uint32_t>(i));
        i += 1 + blk[i];
      }
      for (std::size_t j = starts.size(); j-- > 0;) {
        const uint64_t rank = static_cast<uint64_t>(b) * kBlock + j;
        if (!mark[rank]) continue;
        const uint32_t at2 = starts[j];
        const uint32_t deg = blk[at2];
        for (uint32_t i = 0; i < deg; ++i) {
          const uint32_t par = blk[at2 + 1 + i];
          if (!mark[par]) { mark[par] = true; ++marked; }
        }
      }
    }
    std::fprintf(stderr, "seed p=%lld rank=%lld: %lld ancestors marked  %.0fs\n",
                 (long long)seed, (long long)sr, (long long)marked, Now() - td);

    src.Rewind();
    std::vector<uint32_t> b2(1 << 20);
    std::size_t at = 0, have = 0;
    const auto fl2 = [&]() {
      if (at < have) return true;
      have = src.Read(b2.data(), b2.size());
      at = 0;
      return have > 0;
    };
    uint32_t r = 0;
    int64_t src_total = 0, src_hit = 0, shown = 0;
    while (fl2()) {
      const uint32_t deg = b2[at++];
      for (uint32_t i = 0; i < deg; ++i) { if (!fl2()) break; ++at; }
      if (deg == 0) {
        ++src_total;
        if (mark[r]) ++src_hit;
        else if (shown < 12) {
          if (shown == 0) std::fprintf(stderr, "first sources missed:");
          std::fprintf(stderr, " %lld", (long long)PrimeAt(base, r));
          ++shown;
        }
      }
      ++r;
    }
    if (shown > 0) std::fprintf(stderr, "\n");
    std::fprintf(stderr, "sources reached: %lld of %lld (%.2f%%)\n",
                 (long long)src_hit, (long long)src_total,
                 100.0 * src_hit / src_total);
    return 0;
  }

  std::vector<float> lchains;
  std::vector<uint64_t> at_rank;
  std::vector<uint32_t> at_mask;
  std::vector<double> at_chain;
  std::vector<uint32_t> par;
  std::vector<uint32_t> seed_at;
  std::vector<int64_t> srcs;
  if (!src_path.empty()) {
    std::FILE* sf = std::fopen(src_path.c_str(), "r");
    if (sf == nullptr) { std::fprintf(stderr, "cannot open %s\n", src_path.c_str()); return 1; }
    long long v = 0;
    while (std::fscanf(sf, "%lld", &v) == 1 && srcs.size() < 32) srcs.push_back(v);
    std::fclose(sf);
    for (std::size_t i = 0; i < srcs.size(); ++i) {
      const int64_t r = RankOfPrime(base, srcs[i]);
      if (r < 0) { std::fprintf(stderr, "source %lld not found\n", (long long)srcs[i]); return 1; }
      if (static_cast<std::size_t>(r) >= seed_at.size()) seed_at.resize(r + 1, 0);
      seed_at[r] |= UINT32_C(1) << i;
    }
    std::fprintf(stderr, "seeded %zu sources\n", srcs.size());
  }
  for (const int64_t q : at_primes) {
    const int64_t r = RankOfPrime(base, q);
    if (r < 0) { std::fprintf(stderr, "--at %lld not found\n", (long long)q); return 1; }
    at_rank.push_back(static_cast<uint64_t>(r));
  }
  at_mask.assign(at_rank.size(), 0);
  at_chain.assign(at_rank.size(), 0);

  std::vector<uint32_t> cov;
  std::vector<std::pair<int, uint32_t>> cov_top;
  int cov_cut = 2;

  std::vector<uint32_t> buf(1 << 20);
  std::vector<std::pair<double, uint32_t>> top;
  double cut = 1.0, best = 0;
  uint32_t best_rank = 0, rank = 0;
  int64_t roots = 0, edge_count = 0;
  std::size_t at = 0, have = 0;
  const double t0 = Now();
  double next = report;

  const auto refill = [&]() {
    if (at < have) return true;
    have = src.Read(buf.data(), buf.size());
    at = 0;
    return have > 0;
  };

  while (refill()) {
    const uint32_t deg = buf[at++];
    double ch = 0;
    par.clear();
    for (uint32_t i = 0; i < deg; ++i) {
      if (!refill()) break;
      const uint32_t ix = buf[at++];
      if (!srcs.empty()) par.push_back(ix);
      else ch += std::exp2(static_cast<double>(lchains[ix]));
      ++edge_count;
    }
    if (deg == 0) { ch = 1; ++roots; }
    if (srcs.empty()) lchains.push_back(static_cast<float>(std::log2(ch)));
    if (!srcs.empty()) {
      uint32_t cm = rank < seed_at.size() ? seed_at[rank] : 0;
      for (uint32_t i = 0; i < deg; ++i) cm |= cov[par[i]];
      cov.push_back(cm);
      const int pc = __builtin_popcount(cm);
      if (pc >= cov_cut) {
        cov_top.emplace_back(pc, rank);
        if (cov_top.size() >= 4096) {
          std::sort(cov_top.begin(), cov_top.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
          cov_top.resize(256);
          cov_cut = cov_top.back().first;
        }
      }
    }
    if (ch > best) { best = ch; best_rank = rank; }
    if (ch > cut) {
      top.emplace_back(ch, rank);
      if (top.size() >= 4096) {
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        top.resize(256);
        cut = top.back().first;
      }
    }
    for (std::size_t i = 0; i < at_rank.size(); ++i) {
      if (at_rank[i] != rank) continue;
      at_chain[i] = ch;
      if (!srcs.empty()) at_mask[i] = cov.back();
    }
    if (++rank >= next) {
      next += report;
      std::fprintf(stderr, "rank=%u roots=%lld edges=%lld best=1e%.2f %.1f GB %.0fs\n",
                   rank, static_cast<long long>(roots),
                   static_cast<long long>(edge_count), std::log10(best),
                   Rss() / 1e9, Now() - t0);
    }
  }

  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::fprintf(stderr, "done: primes=%u roots=%lld edges=%lld %.1f GB %.0fs\n",
               rank, static_cast<long long>(roots),
               static_cast<long long>(edge_count), Rss() / 1e9, Now() - t0);
  if (!srcs.empty()) {
    std::sort(cov_top.begin(), cov_top.end(),
              [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second < b.second;
              });
    std::fprintf(stderr, "nodes covering the most of %zu seeded sources:\n",
                 srcs.size());
    for (std::size_t i = 0; i < cov_top.size() && i < 10; ++i) {
      std::fprintf(stderr, "  p=%-14lld covers %d\n",
                   static_cast<long long>(PrimeAt(base, cov_top[i].second)),
                   cov_top[i].first);
    }
    if (cov_top.empty()) std::fprintf(stderr, "  none cover more than one\n");
  }
  for (std::size_t i = 0; i < at_primes.size(); ++i) {
    if (srcs.empty()) {
      std::fprintf(stderr, "\nat p=%lld: chains=1e%.4f\n",
                   static_cast<long long>(at_primes[i]),
                   std::log10(at_chain[i]));
      continue;
    }
    std::fprintf(stderr, "\nat p=%lld:\n", static_cast<long long>(at_primes[i]));
    std::fprintf(stderr, "  covers %d of %zu seeded sources\n",
                 __builtin_popcount(at_mask[i]), srcs.size());
    std::fprintf(stderr, "  misses:");
    int shown = 0;
    for (std::size_t b = 0; b < srcs.size(); ++b) {
      if (((at_mask[i] >> b) & 1) == 0) {
        std::fprintf(stderr, " %lld", static_cast<long long>(srcs[b]));
        ++shown;
      }
    }
    if (shown == 0) std::fprintf(stderr, " none");
    std::fprintf(stderr, "\n");
  }
  std::fprintf(stderr, "most chains:\n");
  for (std::size_t i = 0; i < top.size() && i < 6; ++i) {
    std::fprintf(stderr, "  p=%-13lld chains=1e%.4f\n",
                 static_cast<long long>(PrimeAt(base, top[i].second)),
                 std::log10(top[i].first));
  }
  (void)best_rank;
  return 0;
}
