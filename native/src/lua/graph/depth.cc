#include "primeparts/lua/graph/depth.h"

#include <algorithm>
#include <unordered_map>

namespace primeparts::graph {

namespace {

void Merge(std::vector<int64_t>* into, const std::vector<int64_t>& from) {
  if (into->size() < from.size()) into->resize(from.size(), 0);
  for (std::size_t i = 0; i < from.size(); ++i) (*into)[i] += from[i];
}

}  // namespace

Depth Depth::Of(int64_t root) {
  Depth out;
  out.by_root_.emplace_back(root, std::vector<int64_t>{1});
  return out;
}

Depth Depth::From(int64_t root, std::vector<int64_t> coeffs) {
  Depth out;
  while (!coeffs.empty() && coeffs.back() == 0) coeffs.pop_back();
  if (!coeffs.empty()) out.by_root_.emplace_back(root, std::move(coeffs));
  return out;
}

Depth Depth::Shift() const {
  Depth out;
  out.known_ = known_ == kComplete ? kComplete : known_ + 1;
  out.by_root_.reserve(by_root_.size());
  for (const auto& [root, coeffs] : by_root_) {
    std::vector<int64_t> next(coeffs.size() + 1, 0);
    std::copy(coeffs.begin(), coeffs.end(), next.begin() + 1);
    out.by_root_.emplace_back(root, std::move(next));
  }
  return out;
}

Depth Depth::Add(const Depth& other) const {
  Depth out;
  out.known_ = std::min(known_, other.known_);
  std::size_t a = 0, b = 0;
  while (a < by_root_.size() && b < other.by_root_.size()) {
    if (by_root_[a].first < other.by_root_[b].first) {
      out.by_root_.push_back(by_root_[a++]);
    } else if (other.by_root_[b].first < by_root_[a].first) {
      out.by_root_.push_back(other.by_root_[b++]);
    } else {
      out.by_root_.push_back(by_root_[a]);
      Merge(&out.by_root_.back().second, other.by_root_[b].second);
      ++a;
      ++b;
    }
  }
  while (a < by_root_.size()) out.by_root_.push_back(by_root_[a++]);
  while (b < other.by_root_.size()) out.by_root_.push_back(other.by_root_[b++]);
  return out;
}

Depth Depth::Cut(int32_t d) const {
  Depth out;
  out.known_ = std::min(known_, d);
  if (d < 0) return out;
  for (const auto& [root, coeffs] : by_root_) {
    const std::size_t keep =
        std::min(coeffs.size(), static_cast<std::size_t>(d) + 1);
    std::vector<int64_t> next(coeffs.begin(), coeffs.begin() + keep);
    while (!next.empty() && next.back() == 0) next.pop_back();
    if (!next.empty()) out.by_root_.emplace_back(root, std::move(next));
  }
  return out;
}

std::vector<int64_t> Depth::Roots() const {
  std::vector<int64_t> out;
  out.reserve(by_root_.size());
  for (const auto& [root, coeffs] : by_root_) out.push_back(root);
  return out;
}

const std::vector<int64_t>* Depth::At(int64_t root) const {
  const auto it = std::lower_bound(
      by_root_.begin(), by_root_.end(), root,
      [](const auto& entry, int64_t v) { return entry.first < v; });
  if (it == by_root_.end() || it->first != root) return nullptr;
  return &it->second;
}

int32_t Depth::Min(int64_t root) const {
  const std::vector<int64_t>* coeffs = At(root);
  if (coeffs == nullptr) return -1;
  for (std::size_t i = 0; i < coeffs->size(); ++i) {
    if ((*coeffs)[i] != 0) return static_cast<int32_t>(i);
  }
  return -1;
}

int32_t Depth::Max() const {
  int32_t out = -1;
  for (const auto& [root, coeffs] : by_root_) {
    for (std::size_t i = coeffs.size(); i-- > 0;) {
      if (coeffs[i] == 0) continue;
      out = std::max(out, static_cast<int32_t>(i));
      break;
    }
  }
  return out;
}

int64_t Depth::Count() const {
  int64_t out = 0;
  for (const auto& [root, coeffs] : by_root_) {
    for (const int64_t c : coeffs) out += c;
  }
  return out;
}

int64_t Depth::Cells() const {
  int64_t out = 0;
  for (const auto& [root, coeffs] : by_root_) {
    out += static_cast<int64_t>(coeffs.size());
  }
  return out;
}

namespace {

struct Fold {
  Oracle* oracle = nullptr;
  int64_t ceiling = 0;
  int64_t cells = 0;
  std::unordered_map<int64_t, std::pair<int32_t, Depth>> memo;
  std::string* error = nullptr;

  bool Get(int64_t node, int32_t budget, Depth* out) {
    const auto it = memo.find(node);
    if (it != memo.end()) {
      if (it->second.first >= budget) {
        *out = it->second.second.Cut(budget);
        return true;
      }
      cells -= it->second.second.Cells();
      memo.erase(it);
    }

    std::vector<Edge> parents;
    if (!oracle->Parts(node, &parents, error)) return false;

    if (parents.empty()) {
      *out = Depth::Of(node);
    } else if (budget <= 0) {
      *out = Depth().Cut(0);
    } else {
      Depth acc;
      for (const Edge& e : parents) {
        Depth up;
        if (!Get(e.q, budget - 1, &up)) return false;
        acc = acc.Add(up.Shift());
      }
      *out = std::move(acc);
    }
    cells += out->Cells();
    if (ceiling > 0 && cells > ceiling) {
      *error = "reached the ceiling of " + std::to_string(ceiling) +
               " stored coefficients; raise max_cells or set a depth";
      return false;
    }
    memo[node] = {budget, *out};
    return true;
  }
};

}  // namespace

bool DepthOf(Oracle& oracle, int64_t p, int32_t depth, int64_t max_cells,
             Depth* out, std::string* error) {
  Fold fold;
  fold.oracle = &oracle;
  fold.ceiling = max_cells;
  fold.error = error;
  return fold.Get(p, depth < 0 ? Depth::kComplete : depth, out);
}

}  // namespace primeparts::graph
