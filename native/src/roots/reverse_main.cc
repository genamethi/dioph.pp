#include <getopt.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {

double Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

constexpr int kBuckets = 16;

}  // namespace

// Turns the forward file (per prime: degree, then parent ranks) into the
// reverse one (per prime: child count, then child ranks). Parents are
// partitioned into 16 ranges in a single streaming pass, then each range is
// counting-sorted in memory, so nothing is ever written randomly.
int main(int argc, char** argv) {
  std::string in, out;
  static const option kLong[] = {{"in", required_argument, nullptr, 'i'},
                                 {"out", required_argument, nullptr, 'o'},
                                 {nullptr, 0, nullptr, 0}};
  for (int c; (c = getopt_long(argc, argv, "i:o:", kLong, nullptr)) != -1;) {
    switch (c) {
      case 'i': in = optarg; break;
      case 'o': out = optarg; break;
      default: std::fprintf(stderr, "usage: %s --in F --out F\n", argv[0]); return 2;
    }
  }
  if (in.empty() || out.empty()) {
    std::fprintf(stderr, "usage: %s --in F --out F\n", argv[0]);
    return 2;
  }

  const double t0 = Now();
  std::FILE* f = std::fopen(in.c_str(), "rb");
  if (f == nullptr) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }

  // first pass: how many primes, so the parent range can be split evenly
  uint32_t n = 0;
  {
    std::vector<uint32_t> buf(1 << 20);
    std::size_t at = 0, have = 0;
    const auto fill = [&]() {
      if (at < have) return true;
      have = std::fread(buf.data(), sizeof(uint32_t), buf.size(), f);
      at = 0;
      return have > 0;
    };
    while (fill()) {
      const uint32_t deg = buf[at++];
      for (uint32_t i = 0; i < deg; ++i) { if (!fill()) break; ++at; }
      ++n;
    }
  }
  std::fprintf(stderr, "primes=%u  %.0fs\n", n, Now() - t0);

  const uint32_t span = n / kBuckets + 1;
  std::vector<std::FILE*> part(kBuckets, nullptr);
  for (int b = 0; b < kBuckets; ++b) {
    part[b] = std::fopen((out + ".part" + std::to_string(b)).c_str(), "wb");
    if (part[b] == nullptr) { std::fprintf(stderr, "cannot write part %d\n", b); return 1; }
  }

  // second pass: send each (parent, child) to the bucket owning the parent
  std::rewind(f);
  {
    std::vector<uint32_t> buf(1 << 20);
    std::vector<std::vector<uint32_t>> obuf(kBuckets);
    for (auto& v : obuf) v.reserve(1 << 18);
    std::size_t at = 0, have = 0;
    const auto fill = [&]() {
      if (at < have) return true;
      have = std::fread(buf.data(), sizeof(uint32_t), buf.size(), f);
      at = 0;
      return have > 0;
    };
    uint32_t child = 0;
    int64_t edges = 0;
    while (fill()) {
      const uint32_t deg = buf[at++];
      for (uint32_t i = 0; i < deg; ++i) {
        if (!fill()) break;
        const uint32_t parent = buf[at++];
        const int b = static_cast<int>(parent / span);
        obuf[b].push_back(parent);
        obuf[b].push_back(child);
        ++edges;
        if (obuf[b].size() >= (1 << 18)) {
          std::fwrite(obuf[b].data(), sizeof(uint32_t), obuf[b].size(), part[b]);
          obuf[b].clear();
        }
      }
      ++child;
    }
    for (int b = 0; b < kBuckets; ++b) {
      if (!obuf[b].empty()) {
        std::fwrite(obuf[b].data(), sizeof(uint32_t), obuf[b].size(), part[b]);
      }
      std::fclose(part[b]);
    }
    std::fprintf(stderr, "partitioned %lld edges  %.0fs\n",
                 static_cast<long long>(edges), Now() - t0);
  }
  std::fclose(f);

  std::FILE* o = std::fopen(out.c_str(), "wb");
  if (o == nullptr) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }

  std::vector<uint32_t> obuf;
  obuf.reserve(1 << 20);
  for (int b = 0; b < kBuckets; ++b) {
    const uint32_t lo = static_cast<uint32_t>(b) * span;
    const uint32_t hi = std::min(n, lo + span);
    std::vector<uint32_t> cnt(hi - lo + 1, 0);
    const std::string pn = out + ".part" + std::to_string(b);
    std::FILE* pf = std::fopen(pn.c_str(), "rb");
    std::vector<uint32_t> pair(1 << 20);
    std::size_t got = 0;
    while ((got = std::fread(pair.data(), sizeof(uint32_t), pair.size(), pf)) > 0) {
      for (std::size_t i = 0; i + 1 < got; i += 2) ++cnt[pair[i] - lo];
    }
    std::vector<uint32_t> off(cnt.size() + 1, 0);
    for (std::size_t i = 0; i < cnt.size(); ++i) off[i + 1] = off[i] + cnt[i];
    std::vector<uint32_t> kids(off.back(), 0);
    std::vector<uint32_t> fill_at(off.begin(), off.end() - 1);
    std::rewind(pf);
    while ((got = std::fread(pair.data(), sizeof(uint32_t), pair.size(), pf)) > 0) {
      for (std::size_t i = 0; i + 1 < got; i += 2) {
        kids[fill_at[pair[i] - lo]++] = pair[i + 1];
      }
    }
    std::fclose(pf);
    std::remove(pn.c_str());
    for (uint32_t r = lo; r < hi; ++r) {
      const uint32_t d = cnt[r - lo];
      obuf.push_back(d);
      for (uint32_t i = 0; i < d; ++i) obuf.push_back(kids[off[r - lo] + i]);
      if (obuf.size() >= (1 << 20)) {
        std::fwrite(obuf.data(), sizeof(uint32_t), obuf.size(), o);
        obuf.clear();
      }
    }
    std::fprintf(stderr, "bucket %d/%d done  %.0fs\n", b + 1, kBuckets, Now() - t0);
  }
  if (!obuf.empty()) std::fwrite(obuf.data(), sizeof(uint32_t), obuf.size(), o);
  std::fclose(o);
  std::fprintf(stderr, "reverse written  %.0fs\n", Now() - t0);
  return 0;
}
