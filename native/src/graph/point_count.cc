#include "primeparts/graph/point_count.h"

#include <flint/fmpz.h>
#include <flint/fq_nmod.h>
#include <flint/nmod_poly.h>

#include <cmath>
#include <vector>

#include "primeparts/graph/chain_forms.h"

namespace primeparts::graph {
namespace {

constexpr uint64_t kMaxOrder = uint64_t{1} << 24;

bool Order(int p, int d, uint64_t* q, std::string* error) {
  if (p < 2 || d < 1) {
    *error = "characteristic must be at least 2 and degree at least 1";
    return false;
  }
  uint64_t out = 1;
  for (int i = 0; i < d; ++i) {
    if (out > kMaxOrder / static_cast<uint64_t>(p)) {
      *error = "field order exceeds the supported bound";
      return false;
    }
    out *= static_cast<uint64_t>(p);
  }
  *q = out;
  return true;
}

void EncodeInto(fq_nmod_t out, uint64_t code, int p, int d,
                const fq_nmod_ctx_t ctx) {
  fq_nmod_zero(out, ctx);
  for (int e = 0; e < d; ++e) {
    const ulong digit = static_cast<ulong>(code % static_cast<uint64_t>(p));
    code /= static_cast<uint64_t>(p);
    if (digit != 0) {
      nmod_poly_set_coeff_ui(out, e, digit);
    }
  }
}

uint64_t Encode(const fq_nmod_t v, int p, int d) {
  uint64_t code = 0;
  uint64_t place = 1;
  for (int e = 0; e < d; ++e) {
    code += (nmod_poly_get_coeff_ui(v, e) % static_cast<ulong>(p)) * place;
    place *= static_cast<uint64_t>(p);
  }
  return code;
}

bool ReduceCoefficients(const GiNaC::ex& poly, const GiNaC::symbol& x, int p,
                        std::vector<std::vector<long>>* codes,
                        std::string* error) {
  const GiNaC::ex e = GiNaC::expand(poly);
  const int dx = e.degree(x);
  if (dx < 0) {
    *error = "polynomial has no degree in x";
    return false;
  }
  codes->assign(static_cast<size_t>(dx) + 1, {});
  for (int j = 0; j <= dx; ++j) {
    const GiNaC::ex cj = GiNaC::expand(e.coeff(x, j));
    const int dt = cj.degree(TSymbol());
    std::vector<long>& row = (*codes)[static_cast<size_t>(j)];
    row.assign(static_cast<size_t>(dt < 0 ? 0 : dt) + 1, 0);
    for (int i = 0; i <= dt; ++i) {
      const GiNaC::ex a = cj.coeff(TSymbol(), i);
      if (!a.info(GiNaC::info_flags::integer)) {
        *error = "coefficient is not an integer";
        return false;
      }
      long v = GiNaC::ex_to<GiNaC::numeric>(a).to_long() % p;
      if (v < 0) {
        v += p;
      }
      row[static_cast<size_t>(i)] = v;
    }
  }
  return true;
}

class Evaluator {
 public:
  Evaluator(int p, int d) : p_(p), d_(d) {
    fq_nmod_ctx_init_ui(ctx_, static_cast<ulong>(p), d, "a");
    fq_nmod_init(tmp_, ctx_);
    fq_nmod_init(term_, ctx_);
    fq_nmod_init(y_, ctx_);
    fq_nmod_init(value_, ctx_);
  }

  ~Evaluator() {
    fq_nmod_clear(value_, ctx_);
    fq_nmod_clear(y_, ctx_);
    fq_nmod_clear(term_, ctx_);
    fq_nmod_clear(tmp_, ctx_);
    fq_nmod_ctx_clear(ctx_);
  }

  void Specialize(const std::vector<std::vector<long>>& codes, uint64_t theta) {
    coeff_.assign(codes.size(), 0);
    fq_nmod_t th;
    fq_nmod_t acc;
    fq_nmod_init(th, ctx_);
    fq_nmod_init(acc, ctx_);
    EncodeInto(th, theta, p_, d_, ctx_);
    for (size_t j = 0; j < codes.size(); ++j) {
      fq_nmod_zero(acc, ctx_);
      for (size_t i = 0; i < codes[j].size(); ++i) {
        if (codes[j][i] == 0) {
          continue;
        }
        fq_nmod_pow_ui(term_, th, static_cast<ulong>(i), ctx_);
        fq_nmod_mul_ui(term_, term_, static_cast<ulong>(codes[j][i]), ctx_);
        fq_nmod_add(acc, acc, term_, ctx_);
      }
      coeff_[j] = Encode(acc, p_, d_);
    }
    fq_nmod_clear(acc, ctx_);
    fq_nmod_clear(th, ctx_);
  }

  uint64_t ValueAt(uint64_t code) {
    EncodeInto(y_, code, p_, d_, ctx_);
    fq_nmod_zero(value_, ctx_);
    for (size_t j = coeff_.size(); j-- > 0;) {
      fq_nmod_mul(value_, value_, y_, ctx_);
      EncodeInto(term_, coeff_[j], p_, d_, ctx_);
      fq_nmod_add(value_, value_, term_, ctx_);
    }
    return Encode(value_, p_, d_);
  }

  long TraceOf(uint64_t code) {
    EncodeInto(tmp_, code, p_, d_, ctx_);
    fmpz_t tr;
    fmpz_init(tr);
    fq_nmod_trace(tr, tmp_, ctx_);
    long out = static_cast<long>(fmpz_get_si(tr) % p_);
    fmpz_clear(tr);
    if (out < 0) {
      out += p_;
    }
    return out;
  }

 private:
  int p_;
  int d_;
  fq_nmod_ctx_t ctx_;
  fq_nmod_t tmp_;
  fq_nmod_t term_;
  fq_nmod_t y_;
  fq_nmod_t value_;
  std::vector<uint64_t> coeff_;
};

}  // namespace

bool FiberCounts(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
                 uint64_t theta, FieldPoints* out, std::string* error) {
  uint64_t q = 0;
  if (!Order(p, d, &q, error)) {
    return false;
  }
  std::vector<std::vector<long>> codes;
  if (!ReduceCoefficients(poly, x, p, &codes, error)) {
    return false;
  }
  if (theta >= q) {
    *error = "theta is not an element of the field";
    return false;
  }

  Evaluator ev(p, d);
  ev.Specialize(codes, theta);

  out->p = p;
  out->d = d;
  out->counts.assign(q, 0);
  for (uint64_t code = 0; code < q; ++code) {
    ++out->counts[ev.ValueAt(code)];
  }
  return true;
}

bool Traces(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
            uint64_t theta, TraceSpectrum* out, std::string* error) {
  FieldPoints fp;
  if (!FiberCounts(poly, x, p, d, theta, &fp, error)) {
    return false;
  }
  Evaluator ev(p, d);
  out->p = p;
  out->counts.assign(static_cast<size_t>(p), 0);
  for (uint64_t code = 0; code < fp.counts.size(); ++code) {
    if (fp.counts[code] == 0) {
      continue;
    }
    out->counts[static_cast<size_t>(ev.TraceOf(code))] += fp.counts[code];
  }
  return true;
}

int64_t WeilSum(const TraceSpectrum& s) {
  if (s.p != 2 || s.counts.size() != 2) {
    return 0;
  }
  return s.counts[0] - s.counts[1];
}

double WeilMagnitude(const TraceSpectrum& s) {
  double re = 0.0;
  double im = 0.0;
  for (size_t j = 0; j < s.counts.size(); ++j) {
    const double angle = 2.0 * M_PI * static_cast<double>(j) / s.p;
    re += static_cast<double>(s.counts[j]) * std::cos(angle);
    im += static_cast<double>(s.counts[j]) * std::sin(angle);
  }
  return std::sqrt(re * re + im * im);
}

bool Sum(const std::vector<GiNaC::ex>& polys, const GiNaC::symbol& x, int p,
         int d, uint64_t theta, FieldPoints* out, std::string* error) {
  uint64_t q = 0;
  if (!Order(p, d, &q, error)) {
    return false;
  }
  out->p = p;
  out->d = d;
  out->counts.assign(q, 0);
  for (const GiNaC::ex& poly : polys) {
    FieldPoints one;
    if (!FiberCounts(poly, x, p, d, theta, &one, error)) {
      return false;
    }
    for (size_t i = 0; i < one.counts.size(); ++i) {
      out->counts[i] += one.counts[i];
    }
  }
  return true;
}

int64_t Moment(const FieldPoints& a, int r) {
  int64_t total = 0;
  for (int32_t c : a.counts) {
    int64_t term = 1;
    for (int i = 0; i < r; ++i) {
      term *= c;
    }
    total += term;
  }
  return total;
}

int64_t Correlate(const FieldPoints& a, const FieldPoints& b) {
  if (a.p != b.p || a.d != b.d) {
    return -1;
  }
  int64_t total = 0;
  for (size_t i = 0; i < a.counts.size(); ++i) {
    total += static_cast<int64_t>(a.counts[i]) * b.counts[i];
  }
  return total;
}

int64_t TotalPoints(const FieldPoints& a) {
  int64_t total = 0;
  for (int32_t c : a.counts) {
    total += c;
  }
  return total;
}

}  // namespace primeparts::graph
