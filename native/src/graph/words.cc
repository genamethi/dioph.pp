#include "primeparts/graph/words.h"

#include <flint/ulong_extras.h>

#include <map>
#include <sstream>
#include <unordered_set>

#include "primeparts/core.h"
#include "primeparts/parts_expand.h"

namespace primeparts::graph {
namespace {

struct Enumerator {
  const MaskFn* mask = nullptr;
  const HigherFn* higher = nullptr;
  WordOptions options;
  WordStats* stats = nullptr;
  std::map<int64_t, std::vector<Folded>> memo;

  bool Capped() const { return stats->words >= options.max_words; }

  const std::vector<Folded>& WordsInto(int64_t p, int depth) {
    auto it = memo.find(p);
    if (it != memo.end()) {
      return it->second;
    }
    std::vector<Folded> out;
    std::unordered_set<std::string> seen;

    ConeStats cs;
    const std::vector<int64_t> cone = FlatCone(p, *mask, &cs);
    stats->cone_nodes += cs.nodes;

    for (int64_t v : cone) {
      const uint64_t tail = static_cast<uint64_t>(p - v);

      if ((*mask)(v) == 0 && (*higher)(v).empty()) {
        Folded f;
        f.root = v;
        f.target = p;
        f.a0 = MaskToT(tail);
        const std::string key = WordKey(f);
        if (seen.insert(key).second) {
          out.push_back(std::move(f));
        }
      }

      if (depth >= options.max_blocks) {
        continue;
      }
      for (const HigherEdge& e : (*higher)(v)) {
        ++stats->higher_edges;
        const uint64_t c = (uint64_t{1} << e.m) + tail;
        for (const Folded& base : WordsInto(e.q, depth + 1)) {
          Folded f = base;
          f.target = p;
          Block b;
          b.n = e.n;
          b.c = MaskToT(c);
          f.blocks.push_back(std::move(b));
          const std::string key = WordKey(f);
          if (seen.insert(key).second) {
            out.push_back(std::move(f));
          }
        }
      }
    }

    auto ins = memo.emplace(p, std::move(out));
    return ins.first->second;
  }
};

}  // namespace

std::vector<HigherEdge> ComputedHigher(int64_t p) {
  std::vector<HigherEdge> out;
  if (p < 3) {
    return out;
  }
  const int max_m = 63 - __builtin_clzll(static_cast<uint64_t>(p));
  for (int m = 1; m <= max_m; ++m) {
    const int64_t w = primeparts::PartQ(p, m);
    if (w < 2) {
      continue;
    }
    uint64_t base = 0;
    int32_t exponent = 0;
    if (pp_is_prime_power_u64(static_cast<uint64_t>(w), &base, &exponent) !=
        PP_OK) {
      continue;
    }
    if (exponent >= 2) {
      HigherEdge e;
      e.m = static_cast<int32_t>(m);
      e.n = exponent;
      e.q = static_cast<int64_t>(base);
      out.push_back(e);
    }
  }
  return out;
}

std::string WordKey(const Folded& f) {
  std::ostringstream os;
  os << f.root << '|' << f.a0;
  for (const Block& b : f.blocks) {
    os << '|' << b.n << ':' << b.c;
  }
  return os.str();
}

bool EnumerateWords(int64_t p, const MaskFn& mask, const HigherFn& higher,
                    const WordOptions& options,
                    const std::function<void(const Folded&)>& emit,
                    WordStats* stats, std::string* error) {
  WordStats local;
  if (stats == nullptr) {
    stats = &local;
  }
  *stats = WordStats{};

  if (options.lift != Lift::kCanonical) {
    *error =
        "only the canonical lift is enumerable from the cone; the faithful "
        "lift is indexed by paths and needs a path walk";
    return false;
  }
  if (p < 3) {
    return true;
  }

  Enumerator en;
  en.mask = &mask;
  en.higher = &higher;
  en.options = options;
  en.stats = stats;

  const std::vector<Folded>& words = en.WordsInto(p, 0);
  for (const Folded& f : words) {
    if (stats->words >= options.max_words) {
      stats->capped = true;
      break;
    }
    ++stats->words;
    emit(f);
  }
  return true;
}

}  // namespace primeparts::graph
