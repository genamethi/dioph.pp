#include "primeparts/graph/walk.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace primeparts::graph {

bool Reach(Oracle& oracle, int64_t p, int32_t depth, int64_t max_nodes,
           const TwistTable* twists, Frontier* out, std::string* error) {
  if (p < 3) {
    *error = "p must be at least 3";
    return false;
  }
  if (depth < 0) {
    *error = "depth must be non-negative";
    return false;
  }

  out->levels.clear();
  out->twist_hits.clear();
  out->node_count = 0;
  out->reached_depth = 0;
  out->complete = true;

  std::unordered_set<int64_t> seen;
  std::vector<int64_t> current{p};
  seen.insert(p);
  const int64_t before = oracle.Calls();

  for (int32_t d = 0; d <= depth; ++d) {
    if (current.empty()) break;
    std::sort(current.begin(), current.end());
    out->levels.push_back(Level{.depth = d, .nodes = current});
    out->node_count += static_cast<int64_t>(current.size());
    out->reached_depth = d;

    if (twists != nullptr) {
      for (const int64_t node : current) {
        if (twists->Has(node)) {
          out->twist_hits.push_back(TwistHit{.p = node, .depth = d});
        }
      }
    }
    if (d == depth) break;

    std::vector<int64_t> next;
    std::vector<Part> parts;
    for (const int64_t node : current) {
      if (!oracle.Parts(node, &parts, error)) return false;
      for (const Part& part : parts) {
        if (seen.insert(part.q).second) next.push_back(part.q);
      }
      if (max_nodes > 0 &&
          out->node_count + static_cast<int64_t>(next.size()) > max_nodes) {
        out->complete = false;
        out->oracle_calls = oracle.Calls() - before;
        return true;
      }
    }
    current = std::move(next);
  }

  out->oracle_calls = oracle.Calls() - before;
  return true;
}

namespace {

struct Walker {
  Oracle* oracle = nullptr;
  const ChainOpts* opts = nullptr;
  int64_t floor_p = 0;
  ChainSet* out = nullptr;
  std::string* error = nullptr;
  std::vector<Part> path;

  bool Wanted(int64_t node) const {
    return std::binary_search(opts->targets.begin(), opts->targets.end(), node);
  }

  void Emit(int64_t node) {
    Chain chain;
    chain.edges = path;
    chain.terminal = node;
    for (const Part& e : path) {
      if (e.n > 1) {
        chain.twist_count++;
        chain.degree *= e.n;
      }
    }
    out->chains.push_back(std::move(chain));
  }

  bool Visit(int64_t node, int32_t d) {
    if (out->chains.size() >= static_cast<std::size_t>(opts->limit)) {
      out->truncated = true;
      return true;
    }
    if (node < floor_p) return true;

    std::vector<Part> parts;
    if (!oracle->Parts(node, &parts, error)) return false;
    const bool is_source = parts.empty();

    if (!opts->targets.empty()) {
      if (Wanted(node) && d > 0) Emit(node);
    } else if (is_source || d >= opts->depth) {
      if (!opts->sources_only || is_source) Emit(node);
      return true;
    }
    if (is_source || d >= opts->depth) return true;

    for (const Part& part : parts) {
      path.push_back(part);
      const bool ok = Visit(part.q, d + 1);
      path.pop_back();
      if (!ok) return false;
      if (out->truncated) return true;
    }
    return true;
  }
};

}  // namespace

bool Chains(Oracle& oracle, int64_t p, const ChainOpts& opts, ChainSet* out,
            std::string* error) {
  if (p < 3) {
    *error = "p must be at least 3";
    return false;
  }
  if (opts.limit <= 0) {
    *error = "limit must be positive";
    return false;
  }
  out->chains.clear();
  out->truncated = false;
  const int64_t before = oracle.Calls();

  ChainOpts sorted = opts;
  std::sort(sorted.targets.begin(), sorted.targets.end());
  sorted.targets.erase(std::unique(sorted.targets.begin(), sorted.targets.end()),
                       sorted.targets.end());

  Walker walker;
  walker.oracle = &oracle;
  walker.opts = &sorted;
  walker.floor_p = sorted.targets.empty() ? 0 : sorted.targets.front();
  walker.out = out;
  walker.error = error;
  const bool ok = walker.Visit(p, 0);
  out->oracle_calls = oracle.Calls() - before;
  return ok;
}

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

Word WordOf(const Chain& chain) {
  Word word;
  word.terminal = chain.terminal;
  int64_t acc = 0;
  for (auto it = chain.edges.rbegin(); it != chain.edges.rend(); ++it) {
    const int64_t shift = INT64_C(1) << it->m;
    if (it->n == 1) {
      acc += shift;
      continue;
    }
    if (word.blocks.empty()) {
      word.a0 = acc;
    } else {
      word.blocks.back().c = acc;
    }
    word.blocks.push_back(Block{.n = it->n, .c = 0});
    acc = shift;
  }
  if (word.blocks.empty()) {
    word.a0 = acc;
  } else {
    word.blocks.back().c = acc;
  }
  return word;
}

Expr WordForm(const Word& word, const std::string& symbol) {
  Expr form = Expr::Symbol(symbol).Add(Expr::Int(word.a0));
  for (const Block& b : word.blocks) {
    form = form.Pow(b.n).Add(Expr::Int(b.c));
  }
  return form;
}

Expr ChainForm(const std::vector<Part>& edges, const std::string& symbol) {
  Expr form = Expr::Symbol(symbol);
  for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
    form = Expr::PowerOfTwo(it->m).Add(form.Pow(it->n));
  }
  return form;
}

}  // namespace primeparts::graph
