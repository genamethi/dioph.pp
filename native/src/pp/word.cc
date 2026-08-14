#include "primeparts/pp/word.h"

#include <map>
#include <sstream>
#include <unordered_set>

namespace primeparts::pp {
namespace {

GiNaC::ex RunTerm(int32_t m, Lift lift, uint64_t* accumulated) {
  if (lift == Lift::kFaithful) {
    return GiNaC::pow(Base(), m);
  }
  *accumulated += uint64_t{1} << m;
  return 0;
}

GiNaC::ex FlushRun(Lift lift, uint64_t* accumulated) {
  if (lift == Lift::kFaithful || *accumulated == 0) {
    return 0;
  }
  const GiNaC::ex out = FromMask(*accumulated);
  *accumulated = 0;
  return out;
}

void AddToTail(Word* w, const GiNaC::ex& term) {
  if (w->blocks.empty()) {
    w->a0 += term;
  } else {
    w->blocks.back().c += term;
  }
}

struct Enumerator {
  const MaskFn* mask = nullptr;
  const HigherFn* higher = nullptr;
  WordOptions options;
  WordStats* stats = nullptr;
  std::string* error = nullptr;
  bool failed = false;
  std::vector<Word> aborted;
  std::map<int64_t, std::vector<Word>> memo;

  const std::vector<Word>& Into(int64_t p) {
    auto it = memo.find(p);
    if (it != memo.end()) {
      return it->second;
    }
    std::vector<Word> out;
    std::unordered_set<std::string> seen;

    ConeStats cs;
    const std::vector<int64_t> cone = Cone(p, *mask, &cs);
    stats->cone_nodes += cs.nodes;

    if (options.cone_budget && stats->cone_nodes > *options.cone_budget) {
      *error = "cone budget of " + std::to_string(*options.cone_budget) +
               " nodes exhausted at " + std::to_string(stats->cone_nodes);
      failed = true;
      return aborted;
    }

    for (int64_t v : cone) {
      const uint64_t tail = static_cast<uint64_t>(p - v);
      const std::vector<Ascent> up = (*higher)(v);

      if (up.empty() && (*mask)(v) == 0) {
        Word w;
        w.root = v;
        w.target = p;
        w.a0 = FromMask(tail);
        if (seen.insert(Key(w)).second) {
          out.push_back(std::move(w));
        }
      }

      for (const Ascent& a : up) {
        if (a.q >= v) {
          *error = "ascent from " + std::to_string(v) +
                   " does not descend: " + std::to_string(a.q);
          failed = true;
          return aborted;
        }
        ++stats->ascents;
        const uint64_t c = (uint64_t{1} << a.m) + tail;
        for (const Word& base : Into(a.q)) {
          Word w = base;
          w.target = p;
          Block b;
          b.n = a.n;
          b.c = FromMask(c);
          w.blocks.push_back(std::move(b));
          if (seen.insert(Key(w)).second) {
            out.push_back(std::move(w));
          }
        }
        if (failed) {
          return aborted;
        }
      }
    }

    return memo.emplace(p, std::move(out)).first->second;
  }
};

}  // namespace

Word Fold(const Route& r, Lift lift) {
  Word w;
  w.root = r.root;
  w.target = r.target;
  w.a0 = 0;
  uint64_t run = 0;

  for (const Step& s : r.steps) {
    if (s.n == 1) {
      AddToTail(&w, RunTerm(s.m, lift, &run));
      continue;
    }
    AddToTail(&w, FlushRun(lift, &run));
    Block b;
    b.n = s.n;
    b.c = GiNaC::pow(Base(), s.m);
    w.blocks.push_back(std::move(b));
  }
  AddToTail(&w, FlushRun(lift, &run));

  w.a0 = GiNaC::expand(w.a0);
  for (Block& b : w.blocks) {
    b.c = GiNaC::expand(b.c);
  }
  return w;
}

std::vector<int32_t> Skeleton(const Word& w) {
  std::vector<int32_t> out;
  out.reserve(w.blocks.size());
  for (const Block& b : w.blocks) {
    out.push_back(b.n);
  }
  return out;
}

std::vector<GiNaC::ex> Levels(const Word& w, const GiNaC::symbol& x) {
  std::vector<GiNaC::ex> levels;
  levels.reserve(w.blocks.size() + 1);
  GiNaC::ex p = GiNaC::expand(x + w.a0);
  levels.push_back(p);
  for (const Block& b : w.blocks) {
    p = GiNaC::expand(GiNaC::pow(p, b.n) + b.c);
    levels.push_back(p);
  }
  return levels;
}

GiNaC::ex Composed(const Word& w, const GiNaC::symbol& x) {
  return Levels(w, x).back();
}

bool Closes(const Word& w) {
  const GiNaC::symbol x("x");
  const GiNaC::ex v =
      Evaluate(Composed(w, x)).subs(x == GiNaC::numeric(w.root));
  return GiNaC::expand(v).is_equal(GiNaC::numeric(w.target));
}

int64_t Degree(const Word& w) {
  int64_t d = 1;
  for (const Block& b : w.blocks) {
    d *= b.n;
  }
  return d;
}

int64_t Degree(const Route& r) {
  int64_t d = 1;
  for (const Step& s : r.steps) {
    d *= s.n;
  }
  return d;
}

std::string Key(const Word& w) {
  std::ostringstream os;
  os << w.root << '|' << w.a0;
  for (const Block& b : w.blocks) {
    os << '|' << b.n << ':' << b.c;
  }
  return os.str();
}

bool Enumerate(int64_t p, const MaskFn& mask, const HigherFn& higher,
               const WordOptions& options,
               const std::function<void(const Word&)>& emit, WordStats* stats,
               std::string* error) {
  WordStats local;
  if (stats == nullptr) {
    stats = &local;
  }
  *stats = WordStats{};

  std::string discarded;
  if (error == nullptr) {
    error = &discarded;
  }

  if (options.lift != Lift::kCanonical) {
    *error =
        "only the canonical lift is enumerable from the cone; the faithful "
        "lift is indexed by routes and needs a route walk";
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
  en.error = error;

  const std::vector<Word>& words = en.Into(p);
  if (en.failed) {
    return false;
  }

  for (const Word& w : words) {
    ++stats->words;
    emit(w);
  }
  return true;
}

}  // namespace primeparts::pp
