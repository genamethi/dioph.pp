#include "primeparts/lua/graph/poly.h"

#include <flint/fmpz.h>
#include <flint/fmpz_poly.h>

#include <algorithm>
#include <utility>

#include "fmpz_bigint.h"

namespace primeparts::graph {

struct Poly::Impl {
  fmpz_poly_t p;
  Impl() { fmpz_poly_init(p); }
  ~Impl() { fmpz_poly_clear(p); }
};

Poly::Poly() : impl_(std::make_unique<Impl>()) {}
Poly::~Poly() = default;
Poly::Poly(Poly&&) noexcept = default;
Poly& Poly::operator=(Poly&&) noexcept = default;

Poly::Poly(const Poly& other) : impl_(std::make_unique<Impl>()) {
  fmpz_poly_set(impl_->p, other.impl_->p);
}

Poly& Poly::operator=(const Poly& other) {
  if (this != &other) fmpz_poly_set(impl_->p, other.impl_->p);
  return *this;
}

Poly Poly::X() {
  Poly out;
  fmpz_poly_set_coeff_si(out.impl_->p, 1, 1);
  return out;
}

Poly Poly::Constant(int64_t c) {
  Poly out;
  fmpz_poly_set_coeff_si(out.impl_->p, 0, c);
  return out;
}

Poly Poly::PowerOfTwo(int32_t m) {
  Poly out;
  fmpz_t v;
  fmpz_init(v);
  fmpz_one(v);
  fmpz_mul_2exp(v, v, static_cast<ulong>(m));
  fmpz_poly_set_coeff_fmpz(out.impl_->p, 0, v);
  fmpz_clear(v);
  return out;
}

Poly Poly::He(int64_t n) {
  Poly out;
  if (n >= 0) fmpz_poly_hermite_he(out.impl_->p, static_cast<ulong>(n));
  return out;
}

Poly Poly::Lift(int64_t v) {
  Poly out;
  for (int m = 0; m < 63; ++m) {
    if ((v >> m) & 1) fmpz_poly_set_coeff_si(out.impl_->p, m, 1);
  }
  return out;
}

Poly Poly::Term(int64_t coeff, int64_t exponent) {
  Poly out;
  if (exponent >= 0) {
    fmpz_poly_set_coeff_si(out.impl_->p, static_cast<slong>(exponent), coeff);
  }
  return out;
}

int64_t Poly::Degree() const { return fmpz_poly_degree(impl_->p); }

bool Poly::IsZero() const { return fmpz_poly_is_zero(impl_->p); }

Poly Poly::Add(const Poly& other) const {
  Poly out;
  fmpz_poly_add(out.impl_->p, impl_->p, other.impl_->p);
  return out;
}

Poly Poly::Sub(const Poly& other) const {
  Poly out;
  fmpz_poly_sub(out.impl_->p, impl_->p, other.impl_->p);
  return out;
}

Poly Poly::Mul(const Poly& other) const {
  Poly out;
  fmpz_poly_mul(out.impl_->p, impl_->p, other.impl_->p);
  return out;
}

Poly Poly::Pow(uint64_t e) const {
  Poly out;
  fmpz_poly_pow(out.impl_->p, impl_->p, e);
  return out;
}

Poly Poly::ModCoeffs(const std::string& modulus, std::string* error) const {
  Poly out;
  fmpz_t n;
  fmpz_init(n);
  if (fmpz_set_str(n, modulus.c_str(), 10) != 0 || fmpz_sgn(n) <= 0) {
    fmpz_clear(n);
    *error = "modulus must be a positive integer";
    return out;
  }
  const slong len = fmpz_poly_length(impl_->p);
  fmpz_t c;
  fmpz_init(c);
  for (slong i = 0; i < len; ++i) {
    fmpz_poly_get_coeff_fmpz(c, impl_->p, i);
    fmpz_mod(c, c, n);
    fmpz_poly_set_coeff_fmpz(out.impl_->p, i, c);
  }
  fmpz_clear(c);
  fmpz_clear(n);
  return out;
}

bool Poly::Equals(const Poly& other) const {
  return fmpz_poly_equal(impl_->p, other.impl_->p) != 0;
}

BigInt Poly::Eval(int64_t x) const {
  fmpz_t v, r;
  fmpz_init(v);
  fmpz_init(r);
  fmpz_set_si(v, x);
  fmpz_poly_evaluate_fmpz(r, impl_->p, v);
  BigInt out = ToBigInt(r);
  fmpz_clear(v);
  fmpz_clear(r);
  return out;
}

std::vector<Coeff> Poly::Monomial() const {
  std::vector<Coeff> out;
  const slong len = fmpz_poly_length(impl_->p);
  fmpz_t c;
  fmpz_init(c);
  for (slong i = 0; i < len; ++i) {
    fmpz_poly_get_coeff_fmpz(c, impl_->p, i);
    if (fmpz_is_zero(c)) continue;
    out.push_back(Coeff{.index = static_cast<int64_t>(i), .value = ToBigInt(c)});
  }
  fmpz_clear(c);
  return out;
}

std::vector<Coeff> Poly::Hermite() const {
  std::vector<Coeff> out;
  const slong deg = fmpz_poly_degree(impl_->p);
  if (deg < 0) return out;

  fmpz_poly_t work, basis, scaled;
  fmpz_poly_init(work);
  fmpz_poly_init(basis);
  fmpz_poly_init(scaled);
  fmpz_poly_set(work, impl_->p);

  fmpz_t c;
  fmpz_init(c);
  for (slong d = deg; d >= 0; --d) {
    fmpz_poly_get_coeff_fmpz(c, work, d);
    if (fmpz_is_zero(c)) continue;
    out.push_back(Coeff{.index = static_cast<int64_t>(d), .value = ToBigInt(c)});
    fmpz_poly_hermite_he(basis, static_cast<ulong>(d));
    fmpz_poly_scalar_mul_fmpz(scaled, basis, c);
    fmpz_poly_sub(work, work, scaled);
  }
  fmpz_clear(c);
  fmpz_poly_clear(scaled);
  fmpz_poly_clear(basis);
  fmpz_poly_clear(work);

  std::reverse(out.begin(), out.end());
  return out;
}

std::string Poly::Text(const std::string& var) const {
  char* s = fmpz_poly_get_str_pretty(impl_->p, var.c_str());
  std::string out = s;
  flint_free(s);
  return out;
}

bool PolyFromDecimal(const std::string& decimal, int64_t exponent, Poly* out,
                     std::string* error) {
  if (exponent < 0) {
    *error = "exponent must be non-negative";
    return false;
  }
  fmpz_t c;
  fmpz_init(c);
  if (fmpz_set_str(c, decimal.c_str(), 10) != 0) {
    fmpz_clear(c);
    *error = "cannot parse integer: " + decimal;
    return false;
  }
  Poly built;
  fmpz_poly_set_coeff_fmpz(built.impl_->p, static_cast<slong>(exponent), c);
  fmpz_clear(c);
  *out = std::move(built);
  return true;
}

}  // namespace primeparts::graph
