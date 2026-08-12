#include "primeparts/graph/characters.h"

#include <flint/fmpz.h>
#include <flint/fq_nmod.h>
#include <flint/nmod_poly.h>

#include <cmath>
#include <algorithm>
#include <vector>

namespace primeparts::graph {
namespace {

constexpr uint64_t kMaxOrder = uint64_t{1} << 16;

bool Order(int p, int d, uint64_t* q, std::string* error) {
  if (p < 2 || d < 1) {
    *error = "characteristic must be at least 2 and degree at least 1";
    return false;
  }
  uint64_t out = 1;
  for (int i = 0; i < d; ++i) {
    if (out > kMaxOrder / static_cast<uint64_t>(p)) {
      *error = "field order exceeds the character bound";
      return false;
    }
    out *= static_cast<uint64_t>(p);
  }
  *q = out;
  return true;
}

class Field {
 public:
  Field(int p, int d, uint64_t q) : p_(p), d_(d), q_(q) {
    fq_nmod_ctx_init_ui(ctx_, static_cast<ulong>(p), d, "a");
    fq_nmod_init(a_, ctx_);
    fq_nmod_init(b_, ctx_);
  }

  ~Field() {
    fq_nmod_clear(b_, ctx_);
    fq_nmod_clear(a_, ctx_);
    fq_nmod_ctx_clear(ctx_);
  }

  void Encode(fq_nmod_t out, uint64_t code) const {
    fq_nmod_zero(out, ctx_);
    for (int e = 0; e < d_; ++e) {
      const ulong digit = static_cast<ulong>(code % static_cast<uint64_t>(p_));
      code /= static_cast<uint64_t>(p_);
      if (digit != 0) {
        nmod_poly_set_coeff_ui(out, e, digit);
      }
    }
  }

  uint64_t Decode(const fq_nmod_t v) const {
    uint64_t code = 0;
    uint64_t place = 1;
    for (int e = 0; e < d_; ++e) {
      code += (nmod_poly_get_coeff_ui(v, e) % static_cast<ulong>(p_)) * place;
      place *= static_cast<uint64_t>(p_);
    }
    return code;
  }

  uint64_t Mul(uint64_t u, uint64_t v) {
    Encode(a_, u);
    Encode(b_, v);
    fq_nmod_mul(a_, a_, b_, ctx_);
    return Decode(a_);
  }

  uint64_t Sub(uint64_t u, uint64_t v) {
    Encode(a_, u);
    Encode(b_, v);
    fq_nmod_sub(a_, a_, b_, ctx_);
    return Decode(a_);
  }

  long Trace(uint64_t code) {
    Encode(a_, code);
    fmpz_t tr;
    fmpz_init(tr);
    fq_nmod_trace(tr, a_, ctx_);
    long out = static_cast<long>(fmpz_get_si(tr) % p_);
    fmpz_clear(tr);
    if (out < 0) {
      out += p_;
    }
    return out;
  }

  uint64_t One() {
    fq_nmod_one(a_, ctx_);
    return Decode(a_);
  }

  bool BuildLog(std::vector<int64_t>* log) {
    log->assign(q_, -1);
    const uint64_t one = One();
    if (q_ == 2) {
      (*log)[one] = 0;
      return true;
    }
    for (uint64_t g = 0; g < q_; ++g) {
      if (g == 0 || g == one) {
        continue;
      }
      std::fill(log->begin(), log->end(), int64_t{-1});
      uint64_t cur = one;
      int64_t k = 0;
      bool full = true;
      for (; k < static_cast<int64_t>(q_) - 1; ++k) {
        if ((*log)[cur] != -1) {
          full = false;
          break;
        }
        (*log)[cur] = k;
        cur = Mul(cur, g);
      }
      if (full && cur == one) {
        return true;
      }
    }
    return false;
  }

 private:
  int p_;
  int d_;
  uint64_t q_;
  fq_nmod_ctx_t ctx_;
  fq_nmod_t a_;
  fq_nmod_t b_;
};

}  // namespace

double Magnitude(const Cyclotomic& c) {
  double re = 0.0;
  double im = 0.0;
  for (size_t k = 0; k < c.counts.size(); ++k) {
    const double angle = 2.0 * M_PI * static_cast<double>(k) / c.order;
    re += static_cast<double>(c.counts[k]) * std::cos(angle);
    im += static_cast<double>(c.counts[k]) * std::sin(angle);
  }
  return std::sqrt(re * re + im * im);
}

int64_t Total(const Cyclotomic& c) {
  int64_t total = 0;
  for (int64_t v : c.counts) {
    total += v;
  }
  return total;
}

bool GaussSum(int p, int d, int64_t a, Cyclotomic* out, std::string* error) {
  uint64_t q = 0;
  if (!Order(p, d, &q, error)) {
    return false;
  }
  Field f(p, d, q);
  std::vector<int64_t> log;
  if (!f.BuildLog(&log)) {
    *error = "no multiplicative generator found";
    return false;
  }

  const int64_t m = static_cast<int64_t>(q) - 1;
  out->order = m * p;
  out->counts.assign(static_cast<size_t>(out->order), 0);
  for (uint64_t x = 1; x < q; ++x) {
    const int64_t u = ((a % m) * log[x]) % m;
    const int64_t v = f.Trace(x);
    const int64_t e = (u * p + v * m) % out->order;
    ++out->counts[static_cast<size_t>(e)];
  }
  return true;
}

bool JacobiSum(int p, int d, int64_t a, int64_t b, Cyclotomic* out,
               std::string* error) {
  uint64_t q = 0;
  if (!Order(p, d, &q, error)) {
    return false;
  }
  Field f(p, d, q);
  std::vector<int64_t> log;
  if (!f.BuildLog(&log)) {
    *error = "no multiplicative generator found";
    return false;
  }

  const int64_t m = static_cast<int64_t>(q) - 1;
  const uint64_t one = f.One();
  out->order = m;
  out->counts.assign(static_cast<size_t>(m), 0);
  for (uint64_t x = 1; x < q; ++x) {
    const uint64_t y = f.Sub(one, x);
    if (y == 0) {
      continue;
    }
    const int64_t e =
        (((a % m) * log[x]) % m + ((b % m) * log[y]) % m) % m;
    ++out->counts[static_cast<size_t>((e + m) % m)];
  }
  return true;
}

}  // namespace primeparts::graph
