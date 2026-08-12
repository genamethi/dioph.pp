#include "primeparts/graph/point_count.h"

#include <flint/fq_nmod.h>
#include <flint/nmod_poly.h>

#include <vector>

#include "primeparts/graph/chain_forms.h"

namespace primeparts::graph {
namespace {

void EncodeInto(fq_nmod_t out, uint64_t code, const fq_nmod_ctx_t ctx) {
  fq_nmod_zero(out, ctx);
  while (code != 0) {
    const int e = __builtin_ctzll(code);
    nmod_poly_set_coeff_ui(out, e, 1);
    code &= code - 1;
  }
}

uint64_t Encode(const fq_nmod_t v, int d) {
  uint64_t code = 0;
  for (int e = 0; e < d; ++e) {
    if (nmod_poly_get_coeff_ui(v, e) & 1) {
      code |= uint64_t{1} << e;
    }
  }
  return code;
}

bool ReduceCoefficients(const GiNaC::ex& poly, const GiNaC::symbol& x,
                        std::vector<uint64_t>* codes, std::string* error) {
  const GiNaC::ex e = GiNaC::expand(poly);
  const int dx = e.degree(x);
  if (dx < 0) {
    *error = "polynomial has no degree in x";
    return false;
  }
  codes->assign(static_cast<size_t>(dx) + 1, 0);
  for (int j = 0; j <= dx; ++j) {
    const GiNaC::ex cj = GiNaC::expand(e.coeff(x, j));
    const int dt = cj.degree(TSymbol());
    for (int i = 0; i <= dt; ++i) {
      const GiNaC::ex a = cj.coeff(TSymbol(), i);
      if (!a.info(GiNaC::info_flags::integer)) {
        *error = "coefficient is not an integer";
        return false;
      }
      if (i >= 64) {
        *error = "t-exponent exceeds 63";
        return false;
      }
      if (GiNaC::ex_to<GiNaC::numeric>(a).to_long() % 2 != 0) {
        (*codes)[static_cast<size_t>(j)] ^= uint64_t{1} << i;
      }
    }
  }
  return true;
}

}  // namespace

bool FiberCounts(const GiNaC::ex& poly, const GiNaC::symbol& x, int d,
                 uint64_t theta, FieldPoints* out, std::string* error) {
  if (d < 1 || d > 24) {
    *error = "degree must be in [1, 24]";
    return false;
  }
  std::vector<uint64_t> codes;
  if (!ReduceCoefficients(poly, x, &codes, error)) {
    return false;
  }
  if (theta >> d != 0) {
    *error = "theta is not an element of the field";
    return false;
  }

  fq_nmod_ctx_t ctx;
  fq_nmod_ctx_init_ui(ctx, 2, d, "a");

  fq_nmod_t th;
  fq_nmod_t acc;
  fq_nmod_t y;
  fq_nmod_t value;
  fq_nmod_t term;
  fq_nmod_init(th, ctx);
  fq_nmod_init(acc, ctx);
  fq_nmod_init(y, ctx);
  fq_nmod_init(value, ctx);
  fq_nmod_init(term, ctx);
  EncodeInto(th, theta, ctx);

  std::vector<uint64_t> coeff(codes.size(), 0);
  for (size_t j = 0; j < codes.size(); ++j) {
    fq_nmod_zero(acc, ctx);
    uint64_t bits = codes[j];
    while (bits != 0) {
      const int e = __builtin_ctzll(bits);
      fq_nmod_pow_ui(term, th, static_cast<ulong>(e), ctx);
      fq_nmod_add(acc, acc, term, ctx);
      bits &= bits - 1;
    }
    coeff[j] = Encode(acc, d);
  }

  const uint64_t q = uint64_t{1} << d;
  out->d = d;
  out->counts.assign(q, 0);
  for (uint64_t code = 0; code < q; ++code) {
    EncodeInto(y, code, ctx);
    fq_nmod_zero(value, ctx);
    for (size_t j = coeff.size(); j-- > 0;) {
      fq_nmod_mul(value, value, y, ctx);
      EncodeInto(term, coeff[j], ctx);
      fq_nmod_add(value, value, term, ctx);
    }
    ++out->counts[Encode(value, d)];
  }

  fq_nmod_clear(term, ctx);
  fq_nmod_clear(value, ctx);
  fq_nmod_clear(y, ctx);
  fq_nmod_clear(acc, ctx);
  fq_nmod_clear(th, ctx);
  fq_nmod_ctx_clear(ctx);
  return true;
}

int64_t Correlate(const FieldPoints& a, const FieldPoints& b) {
  if (a.d != b.d) {
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
