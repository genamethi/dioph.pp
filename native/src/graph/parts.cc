#include "primeparts/graph/parts.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "primeparts/core.h"
#include "primeparts/pp_covering.h"

namespace primeparts::graph {

namespace {

uint64_t CovTile(uint64_t word, unsigned field, uint64_t lim) {
  uint64_t x = ((word >> (PP_COV_FIELD * field)) & PP_COV_FIELD_MASK) << 1;
  x |= x << 12;
  x |= x << 24;
  x |= x << 48;
  return x & lim;
}

bool CovPowExponent(unsigned qi, uint64_t q, int32_t* exponent) {
  const int bits = 64 - __builtin_clzll(q);
  const int n = pp_cov_pow_at_bits[qi][bits];
  if (n > 0 && pp_cov_pow[qi][n] == q) {
    *exponent = static_cast<int32_t>(n);
    return true;
  }
  return false;
}

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

  const uint64_t up = static_cast<uint64_t>(p);
  const int max_m = 63 - __builtin_clzll(up);
  const uint64_t word = pp_cov_masks[up % PP_COV_MOD];
  uint64_t lim = max_m >= 63 ? ~UINT64_C(0)
                             : ((UINT64_C(1) << (max_m + 1)) - 1);
  lim &= ~UINT64_C(1);

  for (unsigned qi = 0; qi < PP_COV_NQ; ++qi) {
    uint64_t bits = CovTile(word, qi, lim);
    while (bits != 0) {
      const int m = __builtin_ctzll(bits);
      bits &= bits - 1;
      const uint64_t q = up - (UINT64_C(1) << m);
      int32_t exponent = 0;
      if (q < 2) continue;
      if (CovPowExponent(qi, q, &exponent)) {
        out->push_back(Part{.m = static_cast<int32_t>(m),
                            .n = exponent,
                            .q = static_cast<int64_t>(pp_cov_q[qi])});
      }
    }
  }

  uint64_t bits = CovTile(word, PP_COV_NQ, lim);
  while (bits != 0) {
    const int m = __builtin_ctzll(bits);
    bits &= bits - 1;
    const uint64_t q = up - (UINT64_C(1) << m);
    if (q < 2) continue;
    uint64_t base = 0;
    int32_t exponent = 0;
    const int status = pp_is_prime_power_u64(q, &base, &exponent);
    if (status != PP_OK) {
      *error = pp_status_message(status);
      return false;
    }
    if (exponent > 0) {
      out->push_back(Part{.m = static_cast<int32_t>(m),
                          .n = exponent,
                          .q = static_cast<int64_t>(base)});
    }
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
