#include "primeparts/lua/graph/parts.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "primeparts/lua/lnt.h"

namespace primeparts::graph {

namespace {

class DynamicOracle final : public Oracle {
 public:
  bool Parts(int64_t p, std::vector<Part>* out, std::string* error) override {
    ++calls_;
    return DynamicParts(p, out, error);
  }
  const char* Name() const override { return "dynamic"; }
  int64_t Calls() const override { return calls_; }

 private:
  int64_t calls_ = 0;
};

class CachedOracle final : public Oracle {
 public:
  CachedOracle(std::unique_ptr<Oracle> inner, std::size_t capacity)
      : inner_(std::move(inner)), capacity_(capacity) {}

  bool Parts(int64_t p, std::vector<Part>* out, std::string* error) override {
    const auto it = memo_.find(p);
    if (it != memo_.end()) {
      *out = it->second;
      return true;
    }
    if (!inner_->Parts(p, out, error)) return false;
    if (capacity_ == 0 || memo_.size() < capacity_) memo_.emplace(p, *out);
    return true;
  }

  const char* Name() const override { return inner_->Name(); }
  int64_t Calls() const override { return inner_->Calls(); }

 private:
  std::unique_ptr<Oracle> inner_;
  std::size_t capacity_;
  std::unordered_map<int64_t, std::vector<Part>> memo_;
};

}  // namespace

bool DynamicParts(int64_t p, std::vector<Part>* out, std::string* error) {
  out->clear();
  if (p < 3) {
    *error = "p must be at least 3";
    return false;
  }

  nt::Part found[nt::kMaxParts];
  const int count = nt::PartsOf(static_cast<uint64_t>(p), found);

  out->reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    out->push_back(Part{.m = found[i].m, .n = found[i].n, .q = found[i].q});
  }
  std::sort(out->begin(), out->end(),
            [](const Part& a, const Part& b) { return a.m < b.m; });
  return true;
}

std::unique_ptr<Oracle> MakeDynamicOracle() {
  return std::make_unique<DynamicOracle>();
}

std::unique_ptr<Oracle> MakeCachedOracle(std::unique_ptr<Oracle> inner,
                                         std::size_t capacity) {
  return std::make_unique<CachedOracle>(std::move(inner), capacity);
}

}  // namespace primeparts::graph
