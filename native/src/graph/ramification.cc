#include "primeparts/graph/ramification.h"

#include <flint/fq_nmod.h>
#include <flint/nmod_poly.h>
#include <flint/fq_nmod_poly.h>
#include <flint/fq_nmod_poly_factor.h>

#include "primeparts/graph/chain_forms.h"

namespace primeparts::graph {
namespace {

constexpr uint64_t kMaxOrder = uint64_t{1} << 14;

bool Order(int p, int d, uint64_t* q, std::string* error) {
  if (p < 2 || d < 1) {
    *error = "characteristic must be at least 2 and degree at least 1";
    return false;
  }
  uint64_t out = 1;
  for (int i = 0; i < d; ++i) {
    if (out > kMaxOrder / static_cast<uint64_t>(p)) {
      *error = "field order exceeds the factorisation bound";
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

bool ToFq(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
          uint64_t theta, fq_nmod_poly_t out, const fq_nmod_ctx_t ctx,
          std::string* error) {
  const GiNaC::ex e = GiNaC::expand(poly);
  const int dx = e.degree(x);
  if (dx < 0) {
    *error = "polynomial has no degree in x";
    return false;
  }
  fq_nmod_t th;
  fq_nmod_t acc;
  fq_nmod_t term;
  fq_nmod_init(th, ctx);
  fq_nmod_init(acc, ctx);
  fq_nmod_init(term, ctx);
  EncodeInto(th, theta, p, d, ctx);

  bool ok = true;
  for (int j = 0; j <= dx && ok; ++j) {
    const GiNaC::ex cj = GiNaC::expand(e.coeff(x, j));
    const int dt = cj.degree(TSymbol());
    fq_nmod_zero(acc, ctx);
    for (int i = 0; i <= dt; ++i) {
      const GiNaC::ex a = cj.coeff(TSymbol(), i);
      if (!a.info(GiNaC::info_flags::integer)) {
        *error = "coefficient is not an integer";
        ok = false;
        break;
      }
      long v = GiNaC::ex_to<GiNaC::numeric>(a).to_long() % p;
      if (v < 0) {
        v += p;
      }
      if (v == 0) {
        continue;
      }
      fq_nmod_pow_ui(term, th, static_cast<ulong>(i), ctx);
      fq_nmod_mul_ui(term, term, static_cast<ulong>(v), ctx);
      fq_nmod_add(acc, acc, term, ctx);
    }
    if (ok) {
      fq_nmod_poly_set_coeff(out, j, acc, ctx);
    }
  }

  fq_nmod_clear(term, ctx);
  fq_nmod_clear(acc, ctx);
  fq_nmod_clear(th, ctx);
  return ok;
}

}  // namespace

bool LocalAt(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
             uint64_t theta, uint64_t c, Local* out, std::string* error) {
  uint64_t q = 0;
  if (!Order(p, d, &q, error)) {
    return false;
  }
  if (theta >= q || c >= q) {
    *error = "theta or c is not an element of the field";
    return false;
  }

  fq_nmod_ctx_t ctx;
  fq_nmod_ctx_init_ui(ctx, static_cast<ulong>(p), d, "a");
  fq_nmod_poly_t f;
  fq_nmod_poly_init(f, ctx);

  bool ok = ToFq(poly, x, p, d, theta, f, ctx, error);
  if (ok) {
    fq_nmod_t shift;
    fq_nmod_init(shift, ctx);
    EncodeInto(shift, c, p, d, ctx);
    fq_nmod_poly_t constant;
    fq_nmod_poly_init(constant, ctx);
    fq_nmod_poly_set_coeff(constant, 0, shift, ctx);
    fq_nmod_poly_sub(f, f, constant, ctx);
    fq_nmod_poly_clear(constant, ctx);
    fq_nmod_clear(shift, ctx);

    out->degree = fq_nmod_poly_degree(f, ctx);
    out->places.clear();
    out->ramified = false;
    out->tame = true;
    out->separable = true;

    fq_nmod_poly_t deriv;
    fq_nmod_poly_init(deriv, ctx);
    fq_nmod_poly_derivative(deriv, f, ctx);
    if (fq_nmod_poly_is_zero(deriv, ctx)) {
      out->separable = false;
    }
    fq_nmod_poly_clear(deriv, ctx);

    if (out->degree < 1) {
      *error = "shifted polynomial is constant";
      ok = false;
    } else {
      fq_nmod_poly_factor_t fac;
      fq_nmod_poly_factor_init(fac, ctx);
      fq_nmod_t lead;
      fq_nmod_init(lead, ctx);
      fq_nmod_poly_factor(fac, lead, f, ctx);
      for (slong i = 0; i < fac->num; ++i) {
        Place place;
        place.e = static_cast<int64_t>(fac->exp[i]);
        place.f = static_cast<int64_t>(
            fq_nmod_poly_degree(fac->poly + i, ctx));
        if (place.e > 1 && out->separable) {
          out->ramified = true;
          if (place.e % p == 0) {
            out->tame = false;
          }
        }
        out->places.push_back(place);
      }
      fq_nmod_clear(lead, ctx);
      fq_nmod_poly_factor_clear(fac, ctx);
    }
  }

  fq_nmod_poly_clear(f, ctx);
  fq_nmod_ctx_clear(ctx);
  return ok;
}

bool BranchLocus(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
                 uint64_t theta, std::vector<uint64_t>* out,
                 std::string* error) {
  uint64_t q = 0;
  if (!Order(p, d, &q, error)) {
    return false;
  }
  out->clear();
  for (uint64_t c = 0; c < q; ++c) {
    Local local;
    if (!LocalAt(poly, x, p, d, theta, c, &local, error)) {
      return false;
    }
    if (local.ramified) {
      out->push_back(c);
    }
  }
  return true;
}

}  // namespace primeparts::graph
