#include "primeparts/lua/graph/walk.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>

#include <flint/ulong_extras.h>

#include "primeparts/lua/lnt.h"

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
    std::vector<Edge> parts;
    for (const int64_t node : current) {
      if (!oracle.Parts(node, &parts, error)) return false;
      for (const Edge& part : parts) {
        if (!seen.insert(part.q).second) continue;
        if (max_nodes > 0 &&
            out->node_count + static_cast<int64_t>(next.size()) >= max_nodes) {
          *error = "reached the node ceiling of " + std::to_string(max_nodes) +
                   " at depth " + std::to_string(d + 1);
          return false;
        }
        next.push_back(part.q);
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
  std::vector<Edge> path;

  bool Wanted(int64_t node) const {
    return std::binary_search(opts->targets.begin(), opts->targets.end(), node);
  }

  void Emit(int64_t node) {
    Chain chain;
    chain.edges = path;
    chain.terminal = node;
    for (const Edge& e : path) {
      if (e.n > 1) {
        chain.twist_count++;
        chain.degree *= e.n;
      }
    }
    out->chains.push_back(std::move(chain));
  }

  bool Visit(int64_t node, int32_t d) {
    if (node < floor_p) return true;

    std::vector<Edge> parts;
    if (!oracle->Parts(node, &parts, error)) return false;
    const bool is_source = parts.empty();

    if (!opts->targets.empty()) {
      if (Wanted(node) && d > 0) Emit(node);
    } else if (is_source || d >= opts->depth) {
      if (!opts->sources_only || is_source) Emit(node);
      return true;
    }
    if (is_source || d >= opts->depth) return true;

    for (const Edge& part : parts) {
      path.push_back(part);
      const bool ok = Visit(part.q, d + 1);
      path.pop_back();
      if (!ok) return false;
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
  out->chains.clear();
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

namespace {

int64_t Root(int64_t hi, int32_t e) {
  if (e <= 1) return hi;
  if (e > 63) return 2;
  ulong rem = 0;
  return static_cast<int64_t>(
      n_rootrem(&rem, static_cast<ulong>(hi), static_cast<ulong>(e)));
}

void Cone(int64_t start, int64_t cap, std::vector<int64_t>* out) {
  out->clear();
  if (start > cap) return;

  std::unordered_set<int64_t> seen{start};
  std::vector<int64_t> frontier{start};
  std::vector<nt::Edge> edges;
  out->push_back(start);

  while (!frontier.empty()) {
    std::vector<int64_t> next;
    for (const int64_t q : frontier) {
      nt::InvOf(static_cast<uint64_t>(q), static_cast<uint64_t>(cap), &edges);
      for (const nt::Edge& e : edges) {
        if (e.n != 1) continue;
        if (seen.insert(e.p).second) {
          next.push_back(e.p);
          out->push_back(e.p);
        }
      }
    }
    frontier = std::move(next);
  }
}

struct SkelWalk {
  const std::vector<int32_t>* skel = nullptr;
  std::vector<int64_t> bound;
  SkelSet* out = nullptr;
  int64_t root = 0;
  int64_t a0 = 0;
  std::vector<Block> blocks;

  void Run(std::size_t level, int64_t entry) {
    const int32_t n = (*skel)[level];
    std::vector<int64_t> cone;
    std::vector<nt::Edge> edges;
    Cone(entry, bound[level], &cone);
    out->nodes += static_cast<int64_t>(cone.size());

    for (const int64_t v : cone) {
      const int64_t delta = v - entry;
      if (level == 0) {
        a0 = delta;
      } else {
        blocks[level - 1].c += delta;
      }

      nt::InvOf(static_cast<uint64_t>(v),
                static_cast<uint64_t>(bound[level + 1]), &edges);
      for (const nt::Edge& e : edges) {
        if (e.n != n) continue;
        blocks.push_back(Block{.n = n, .c = INT64_C(1) << e.m});
        if (level + 1 == skel->size()) {
          Word word;
          word.terminal = root;
          word.a0 = a0;
          word.blocks = blocks;
          out->hits.push_back(SkelHit{.word = std::move(word), .p = e.p});
        } else {
          Run(level + 1, e.p);
        }
        blocks.pop_back();
      }

      if (level == 0) {
        a0 = 0;
      } else {
        blocks[level - 1].c -= delta;
      }
    }
  }
};

}  // namespace

bool Skeleton(const SkelOpts& opts, SkelSet* out, std::string* error) {
  if (opts.skeleton.empty()) {
    *error = "skeleton must have at least one block";
    return false;
  }
  if (opts.hi < 3) {
    *error = "hi must be at least 3";
    return false;
  }
  for (const int32_t n : opts.skeleton) {
    if (n < 2) {
      *error = "every skeleton entry must be at least 2";
      return false;
    }
  }

  out->hits.clear();
  out->roots.clear();
  out->nodes = 0;

  const std::size_t k = opts.skeleton.size();
  out->bounds.assign(k + 1, opts.hi);
  int32_t suffix = 1;
  for (std::size_t i = k; i-- > 0;) {
    suffix = suffix > 63 / opts.skeleton[i] ? 64 : suffix * opts.skeleton[i];
    out->bounds[i] = Root(opts.hi, suffix);
  }

  if (opts.roots.empty()) {
    nt::Edge edges[nt::kMaxEdges];
    for (int64_t r = 3; r <= out->bounds[0]; r += 2) {
      if (n_is_prime(static_cast<ulong>(r)) == 0) continue;
      if (nt::PartsOf(static_cast<uint64_t>(r), edges) == 0) {
        out->roots.push_back(r);
      }
    }
  } else {
    for (const int64_t r : opts.roots) {
      if (r >= 3 && r <= out->bounds[0]) out->roots.push_back(r);
    }
    std::sort(out->roots.begin(), out->roots.end());
    out->roots.erase(std::unique(out->roots.begin(), out->roots.end()),
                     out->roots.end());
  }

  SkelWalk walk;
  walk.skel = &opts.skeleton;
  walk.bound = out->bounds;
  walk.out = out;
  for (const int64_t r : out->roots) {
    walk.root = r;
    walk.Run(0, r);
  }
  return true;
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

Expr ChainForm(const std::vector<Edge>& edges, const std::string& symbol) {
  Expr form = Expr::Symbol(symbol);
  for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
    form = Expr::PowerOfTwo(it->m).Add(form.Pow(it->n));
  }
  return form;
}

}  // namespace primeparts::graph
