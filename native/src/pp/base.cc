#include "primeparts/pp/base.h"

namespace primeparts::pp {

const GiNaC::symbol& Base() {
  static const GiNaC::symbol t("t");
  return t;
}

GiNaC::ex FromMask(uint64_t mask) {
  GiNaC::ex sum = 0;
  while (mask != 0) {
    const int e = __builtin_ctzll(mask);
    sum += GiNaC::pow(Base(), e);
    mask &= mask - 1;
  }
  return sum;
}

bool ToMask(const GiNaC::ex& c, uint64_t* mask) {
  const GiNaC::ex e = GiNaC::expand(c);
  const int deg = e.degree(Base());
  if (deg < 0 || deg > 63) {
    return false;
  }
  uint64_t out = 0;
  for (int i = 0; i <= deg; ++i) {
    const GiNaC::ex a = e.coeff(Base(), i);
    if (a.is_zero()) {
      continue;
    }
    if (!a.info(GiNaC::info_flags::integer) || !a.is_equal(1)) {
      return false;
    }
    out |= uint64_t{1} << i;
  }
  *mask = out;
  return true;
}

GiNaC::ex Evaluate(const GiNaC::ex& e) {
  return GiNaC::expand(e.subs(Base() == 2));
}

}  // namespace primeparts::pp
