#include "primeparts/graph/chain_forms.h"

#include <string>

namespace primeparts::graph {

const GiNaC::symbol& TSymbol() {
  static const GiNaC::symbol t("t");
  return t;
}

GiNaC::ex MaskToT(uint64_t mask) {
  GiNaC::ex sum = 0;
  while (mask != 0) {
    const int e = __builtin_ctzll(mask);
    sum += GiNaC::pow(TSymbol(), e);
    mask &= mask - 1;
  }
  return sum;
}

bool TToMask(const GiNaC::ex& c, uint64_t* mask) {
  const GiNaC::ex e = GiNaC::expand(c);
  const int deg = e.degree(TSymbol());
  if (deg < 0 || deg > 63) {
    return false;
  }
  uint64_t out = 0;
  for (int i = 0; i <= deg; ++i) {
    const GiNaC::ex a = e.coeff(TSymbol(), i);
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

GiNaC::ex EvalAtTwo(const GiNaC::ex& e) {
  return GiNaC::expand(e.subs(TSymbol() == 2));
}

int64_t Degree(const Word& w) {
  int64_t d = 1;
  for (const Step& s : w.steps) {
    d *= s.n;
  }
  return d;
}

std::vector<GiNaC::symbol> ChainSymbols(const Word& w) {
  std::vector<GiNaC::symbol> y;
  y.reserve(w.steps.size() + 1);
  for (size_t i = 0; i <= w.steps.size(); ++i) {
    y.emplace_back("y" + std::to_string(i));
  }
  return y;
}

GiNaC::ex StepGenerator(const Step& s, const GiNaC::ex& y_in,
                        const GiNaC::ex& y_out) {
  return GiNaC::expand(y_out - GiNaC::pow(y_in, s.n) -
                       GiNaC::pow(TSymbol(), s.m));
}

std::vector<GiNaC::ex> Uncollapsed(const Word& w,
                                   const std::vector<GiNaC::symbol>& y) {
  std::vector<GiNaC::ex> gens;
  if (y.size() != w.steps.size() + 1) {
    return gens;
  }
  gens.reserve(w.steps.size());
  for (size_t i = 0; i < w.steps.size(); ++i) {
    gens.push_back(StepGenerator(w.steps[i], y[i], y[i + 1]));
  }
  return gens;
}

std::vector<GiNaC::ex> Graded(const Word& w, const GiNaC::symbol& x) {
  std::vector<GiNaC::ex> levels;
  levels.reserve(w.steps.size() + 1);
  GiNaC::ex p = x;
  levels.push_back(p);
  for (const Step& s : w.steps) {
    p = GiNaC::expand(GiNaC::pow(p, s.n) + GiNaC::pow(TSymbol(), s.m));
    levels.push_back(p);
  }
  return levels;
}

GiNaC::ex Composed(const Word& w, const GiNaC::symbol& x) {
  return Graded(w, x).back();
}

GiNaC::ex FiberProduct(const Word& a, const Word& b, const GiNaC::symbol& y,
                       const GiNaC::symbol& y_other) {
  return GiNaC::expand(Composed(a, y) - Composed(b, y_other));
}

bool ClosesAtTwo(const Word& w) {
  const GiNaC::symbol x("x");
  const GiNaC::ex v =
      EvalAtTwo(Composed(w, x)).subs(x == GiNaC::numeric(w.root));
  return GiNaC::expand(v).is_equal(GiNaC::numeric(w.target));
}

}  // namespace primeparts::graph
