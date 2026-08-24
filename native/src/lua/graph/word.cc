#include "primeparts/lua/graph/word.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace primeparts::graph {

int64_t Word::Degree() const {
  int64_t d = 1;
  for (const Block& b : blocks) d *= b.n;
  return d;
}

int32_t Word::Grade() const { return static_cast<int32_t>(blocks.size()); }

int64_t Word::OddDegree() const {
  int64_t d = Degree();
  while (d % 2 == 0) d /= 2;
  return d;
}

std::string Word::Key() const {
  std::string out = std::to_string(terminal) + ":" + std::to_string(a0);
  for (const Block& b : blocks) {
    out += "|" + std::to_string(b.n) + "," + std::to_string(b.c);
  }
  return out;
}

Word Word::Step(int32_t m, int32_t n) const {
  Word out = *this;
  const int64_t shift = INT64_C(1) << m;
  if (n == 1) {
    if (out.blocks.empty()) {
      out.a0 += shift;
    } else {
      out.blocks.back().c += shift;
    }
    return out;
  }
  out.blocks.push_back(Block{.n = n, .c = shift});
  return out;
}

bool operator<(const Word& a, const Word& b) {
  if (a.terminal != b.terminal) return a.terminal < b.terminal;
  if (a.a0 != b.a0) return a.a0 < b.a0;
  if (a.blocks.size() != b.blocks.size()) {
    return a.blocks.size() < b.blocks.size();
  }
  for (std::size_t i = 0; i < a.blocks.size(); ++i) {
    if (a.blocks[i].n != b.blocks[i].n) return a.blocks[i].n < b.blocks[i].n;
    if (a.blocks[i].c != b.blocks[i].c) return a.blocks[i].c < b.blocks[i].c;
  }
  return false;
}

Expr WordForm(const Word& word, const std::string& symbol,
              const std::string& base) {
  const auto konst = [&base](int64_t v) {
    return base.empty() ? Expr::Int(v) : Expr::Lift(v, base);
  };
  Expr form = Expr::Symbol(symbol).Add(konst(word.a0));
  for (const Block& b : word.blocks) {
    form = form.Pow(b.n).Add(konst(b.c));
  }
  return form;
}

namespace {

void Merge(std::vector<int64_t>* into, const std::vector<int64_t>& from) {
  if (into->size() < from.size()) into->resize(from.size(), 0);
  for (std::size_t i = 0; i < from.size(); ++i) (*into)[i] += from[i];
}

std::vector<int64_t> Shifted(const std::vector<int64_t>& v) {
  std::vector<int64_t> out(v.size() + 1, 0);
  std::copy(v.begin(), v.end(), out.begin() + 1);
  return out;
}

}  // namespace

Words Words::Of(int64_t root) {
  Words out;
  Word unit;
  unit.terminal = root;
  out.by_word_.emplace(std::move(unit), std::vector<int64_t>{1});
  return out;
}

Words Words::Step(int32_t m, int32_t n) const {
  Words out;
  out.known_ = known_ == Depth::kComplete ? Depth::kComplete : known_ + 1;
  for (const auto& [word, counts] : by_word_) {
    Merge(&out.by_word_[word.Step(m, n)], Shifted(counts));
  }
  return out;
}

Words Words::Add(const Words& other) const {
  Words out = *this;
  out.known_ = std::min(known_, other.known_);
  for (const auto& [word, counts] : other.by_word_) {
    Merge(&out.by_word_[word], counts);
  }
  return out;
}

Words Words::Cut(int32_t d) const {
  Words out;
  out.known_ = std::min(known_, d);
  if (d < 0) return out;
  for (const auto& [word, counts] : by_word_) {
    const std::size_t keep =
        std::min(counts.size(), static_cast<std::size_t>(d) + 1);
    std::vector<int64_t> next(counts.begin(), counts.begin() + keep);
    while (!next.empty() && next.back() == 0) next.pop_back();
    if (!next.empty()) out.by_word_.emplace(word, std::move(next));
  }
  return out;
}

int64_t Words::Count() const {
  int64_t out = 0;
  for (const auto& [word, counts] : by_word_) {
    for (const int64_t c : counts) out += c;
  }
  return out;
}

int64_t Words::Cells() const {
  int64_t out = 0;
  for (const auto& [word, counts] : by_word_) {
    out += static_cast<int64_t>(counts.size() + 2 * word.blocks.size() + 2);
  }
  return out;
}

Depth Words::Grade() const {
  std::map<int64_t, std::vector<int64_t>> by_root;
  for (const auto& [word, counts] : by_word_) {
    Merge(&by_root[word.terminal], counts);
  }
  Depth out;
  for (auto& [root, counts] : by_root) {
    out = out.Add(Depth::From(root, std::move(counts)));
  }
  return out.Cut(known_);
}

namespace {

struct Fold {
  Oracle* oracle = nullptr;
  const std::vector<int64_t>* targets = nullptr;
  int64_t floor_p = 0;
  int64_t ceiling = 0;
  int64_t cells = 0;
  std::unordered_map<int64_t, std::pair<int32_t, Words>> memo;
  std::string* error = nullptr;

  bool Get(int64_t node, int32_t budget, Words* out) {
    const auto it = memo.find(node);
    if (it != memo.end()) {
      if (it->second.first >= budget) {
        *out = it->second.second.Cut(budget);
        return true;
      }
      cells -= it->second.second.Cells();
      memo.erase(it);
    }

    if (!targets->empty() &&
        std::binary_search(targets->begin(), targets->end(), node)) {
      *out = Words::Of(node);
      memo[node] = {Depth::kComplete, *out};
      cells += out->Cells();
      return true;
    }
    if (node < floor_p) {
      *out = Words();
      memo[node] = {Depth::kComplete, *out};
      return true;
    }

    std::vector<Edge> parents;
    if (!oracle->Parts(node, &parents, error)) return false;

    if (parents.empty()) {
      *out = targets->empty() ? Words::Of(node) : Words();
    } else if (budget <= 0) {
      *out = Words().Cut(0);
    } else {
      Words acc;
      for (const Edge& e : parents) {
        Words up;
        if (!Get(e.q, budget - 1, &up)) return false;
        acc = acc.Add(up.Step(e.m, e.n));
      }
      *out = std::move(acc);
    }

    cells += out->Cells();
    if (ceiling > 0 && cells > ceiling) {
      *error = "reached the ceiling of " + std::to_string(ceiling) +
               " stored cells; raise max_cells or set a depth";
      return false;
    }
    memo[node] = {budget, *out};
    return true;
  }
};

}  // namespace

bool WordsOf(Oracle& oracle, int64_t p, const WordOpts& opts, Words* out,
             std::string* error) {
  std::vector<int64_t> targets = opts.targets;
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

  Fold fold;
  fold.oracle = &oracle;
  fold.targets = &targets;
  fold.floor_p = targets.empty() ? 0 : targets.front();
  fold.ceiling = opts.max_cells;
  fold.error = error;
  return fold.Get(p, opts.depth < 0 ? Depth::kComplete : opts.depth, out);
}

}  // namespace primeparts::graph
