#include "primeparts/graph/hermite_modl.h"

#include <flint/nmod_poly.h>
#include <flint/nmod_poly_factor.h>
#include <flint/ulong_extras.h>

#include <algorithm>

namespace primeparts::graph {

GiNaC::ex He(int n, const GiNaC::symbol& x) {
  GiNaC::ex a = 1;
  GiNaC::ex b = x;
  if (n == 0) return a;
  for (int k = 1; k < n; ++k) {
    GiNaC::ex c = GiNaC::expand(x * b - k * a);
    a = b;
    b = c;
  }
  return b;
}

std::map<int, GiNaC::ex> ToHermite(GiNaC::ex P, const GiNaC::symbol& x) {
  std::map<int, GiNaC::ex> out;
  P = GiNaC::expand(P);
  while (!P.is_zero()) {
    int d = P.degree(x);
    GiNaC::ex c = P.lcoeff(x);
    out[d] = c;
    P = GiNaC::expand(P - c * He(d, x));
    if (d == 0) break;
  }
  return out;
}

namespace {

void BuildHermite(int n, uint64_t ell, nmod_poly_t out) {
  nmod_poly_t a, t;
  nmod_poly_init(a, ell);
  nmod_poly_init(t, ell);
  nmod_poly_zero(out);
  nmod_poly_set_coeff_ui(a, 0, 1);
  if (n == 0) {
    nmod_poly_set(out, a);
  } else {
    nmod_poly_set_coeff_ui(out, 1, 1);
    for (int k = 1; k < n; ++k) {
      nmod_poly_shift_left(t, out, 1);
      nmod_poly_scalar_mul_nmod(a, a, k % ell);
      nmod_poly_sub(a, t, a);
      nmod_poly_swap(a, out);
    }
  }
  nmod_poly_clear(a);
  nmod_poly_clear(t);
}

std::vector<uint64_t> Roots(const nmod_poly_t P, uint64_t ell) {
  std::vector<uint64_t> out;
  if (nmod_poly_degree(P) < 1) return out;
  nmod_poly_t xx, xq, g;
  nmod_poly_init(xx, ell);
  nmod_poly_init(xq, ell);
  nmod_poly_init(g, ell);
  nmod_poly_set_coeff_ui(xx, 1, 1);
  nmod_poly_powmod_ui_binexp(xq, xx, ell, P);
  nmod_poly_sub(xq, xq, xx);
  nmod_poly_gcd(g, P, xq);
  if (nmod_poly_degree(g) >= 1) {
    nmod_poly_factor_t fac;
    nmod_poly_factor_init(fac);
    nmod_poly_factor(fac, g);
    for (slong i = 0; i < fac->num; ++i) {
      if (nmod_poly_degree(fac->p + i) != 1) continue;
      const uint64_t c = nmod_poly_get_coeff_ui(fac->p + i, 0);
      out.push_back(c == 0 ? 0 : ell - c);
    }
    nmod_poly_factor_clear(fac);
  }
  nmod_poly_clear(xx);
  nmod_poly_clear(xq);
  nmod_poly_clear(g);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

HermiteRootsReport HermiteRootsModL(int n, uint64_t ell) {
  HermiteRootsReport r;
  r.ell = ell;
  r.n = n;
  nmod_poly_t P, core;
  nmod_poly_init(P, ell);
  nmod_poly_init(core, ell);
  BuildHermite(n, ell, P);
  for (uint64_t root : Roots(P, ell))
    if (root != 0) r.roots.push_back(root);
  if (n % 2 == 1) nmod_poly_shift_right(P, P, 1);
  for (slong i = 0; i <= nmod_poly_degree(P); i += 2)
    nmod_poly_set_coeff_ui(core, i / 2, nmod_poly_get_coeff_ui(P, i));
  r.core_roots = Roots(core, ell);
  std::vector<uint64_t> predicted;
  for (uint64_t t : r.core_roots) {
    if (t == 0 || n_jacobi_unsigned(t, ell) != 1) continue;
    const uint64_t s = n_sqrtmod(t, ell);
    predicted.push_back(s);
    predicted.push_back(ell - s);
  }
  std::sort(predicted.begin(), predicted.end());
  r.criterion_match = predicted == r.roots;
  nmod_poly_clear(P);
  nmod_poly_clear(core);
  return r;
}

std::vector<uint64_t> PrimitiveMersenneFactors(int d) {
  std::vector<uint64_t> out;
  if (d < 2 || d > 63) return out;
  const uint64_t mersenne = (1ULL << d) - 1;
  n_factor_t fac;
  n_factor_init(&fac);
  n_factor(&fac, mersenne, 0);
  for (int i = 0; i < fac.num; ++i) {
    const uint64_t p = fac.p[i];
    const uint64_t pinv = n_preinvert_limb(p);
    bool primitive = true;
    for (int e = 1; e < d; ++e) {
      if (d % e != 0) continue;
      if (n_powmod2_ui_preinv(2 % p, e, p, pinv) == 1) {
        primitive = false;
        break;
      }
    }
    if (primitive) out.push_back(p);
  }
  return out;
}

uint64_t HermiteEvalModL(int n, uint64_t value, uint64_t ell) {
  const uint64_t ninv = n_preinvert_limb(ell);
  const uint64_t x = value % ell;
  uint64_t a = 1 % ell;
  uint64_t b = x;
  if (n == 0) return a;
  for (int k = 1; k < n; ++k) {
    const uint64_t xb = n_mulmod2_preinv(x, b, ell, ninv);
    const uint64_t ka = n_mulmod2_preinv(static_cast<uint64_t>(k) % ell, a, ell, ninv);
    const uint64_t next = n_submod(xb, ka, ell);
    a = b;
    b = next;
  }
  return b;
}

}  // namespace primeparts::graph
