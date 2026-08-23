#include "primeparts/graph/walk.h"

#include <algorithm>
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
  int32_t depth = 0;
  int64_t limit = 0;
  bool sources_only = false;
  ChainSet* out = nullptr;
  std::string* error = nullptr;
  std::vector<Part> path;

  bool Visit(int64_t node, int32_t d) {
    if (out->chains.size() >= static_cast<std::size_t>(limit)) {
      out->truncated = true;
      return true;
    }
    std::vector<Part> parts;
    if (!oracle->Parts(node, &parts, error)) return false;

    const bool is_source = parts.empty();
    const bool at_cut = d >= depth;
    if (is_source || at_cut) {
      if (!sources_only || is_source) {
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
      return true;
    }

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

bool Chains(Oracle& oracle, int64_t p, int32_t depth, int64_t limit,
            bool sources_only, ChainSet* out, std::string* error) {
  if (p < 3) {
    *error = "p must be at least 3";
    return false;
  }
  if (limit <= 0) {
    *error = "limit must be positive";
    return false;
  }
  out->chains.clear();
  out->truncated = false;
  const int64_t before = oracle.Calls();

  Walker walker;
  walker.oracle = &oracle;
  walker.depth = depth;
  walker.limit = limit;
  walker.sources_only = sources_only;
  walker.out = out;
  walker.error = error;
  const bool ok = walker.Visit(p, 0);
  out->oracle_calls = oracle.Calls() - before;
  return ok;
}

Expr ChainForm(const std::vector<Part>& edges, const std::string& symbol) {
  Expr form = Expr::Symbol(symbol);
  for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
    form = Expr::PowerOfTwo(it->m).Add(form.Pow(it->n));
  }
  return form;
}

}  // namespace primeparts::graph
