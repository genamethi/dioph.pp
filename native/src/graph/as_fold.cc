#include "primeparts/graph/as_fold.h"

#include <map>

#include "primeparts/graph/chain_forms.h"

namespace primeparts::graph {
namespace {

bool OddCoefficient(const GiNaC::ex& a) {
  if (!a.info(GiNaC::info_flags::integer)) {
    return false;
  }
  return GiNaC::ex_to<GiNaC::numeric>(a).to_long() % 2 != 0;
}

}  // namespace

GiNaC::ex ReduceModTwo(const GiNaC::ex& c) {
  const GiNaC::ex e = GiNaC::expand(c);
  const int deg = e.degree(TSymbol());
  GiNaC::ex out = 0;
  for (int i = 0; i <= deg; ++i) {
    if (OddCoefficient(e.coeff(TSymbol(), i))) {
      out += GiNaC::pow(TSymbol(), i);
    }
  }
  return GiNaC::expand(out);
}

bool IsSquareModTwo(const GiNaC::ex& c) {
  const GiNaC::ex e = ReduceModTwo(c);
  const int deg = e.degree(TSymbol());
  for (int i = 1; i <= deg; i += 2) {
    if (!e.coeff(TSymbol(), i).is_zero()) {
      return false;
    }
  }
  return true;
}

GiNaC::ex SqrtModTwo(const GiNaC::ex& c) {
  const GiNaC::ex e = ReduceModTwo(c);
  const int deg = e.degree(TSymbol());
  GiNaC::ex out = 0;
  for (int i = 0; i <= deg; i += 2) {
    if (!e.coeff(TSymbol(), i).is_zero()) {
      out += GiNaC::pow(TSymbol(), i / 2);
    }
  }
  return GiNaC::expand(out);
}

GiNaC::ex ArtinSchreierReduce(const GiNaC::ex& poly, const GiNaC::symbol& x) {
  const GiNaC::ex e = GiNaC::expand(poly);
  const int dx = e.degree(x);
  std::map<int, GiNaC::ex> coeff;
  for (int j = 0; j <= dx; ++j) {
    const GiNaC::ex c = ReduceModTwo(e.coeff(x, j));
    if (!c.is_zero()) {
      coeff[j] = c;
    }
  }

  for (;;) {
    while (!coeff.empty() && coeff.rbegin()->second.is_zero()) {
      coeff.erase(std::prev(coeff.end()));
    }
    if (coeff.empty()) {
      return 0;
    }
    const int d = coeff.rbegin()->first;
    if (d < 2 || (d & 1) != 0) {
      break;
    }
    if (!IsSquareModTwo(coeff[d])) {
      break;
    }
    const GiNaC::ex root = SqrtModTwo(coeff[d]);
    coeff.erase(d);
    auto it = coeff.find(d / 2);
    if (it == coeff.end()) {
      coeff[d / 2] = root;
    } else {
      it->second = ReduceModTwo(it->second + root);
    }
  }

  GiNaC::ex out = 0;
  for (const auto& [j, c] : coeff) {
    out += c * GiNaC::pow(x, j);
  }
  return GiNaC::expand(out);
}

int SwanAtInfinity(const GiNaC::ex& poly, const GiNaC::symbol& x) {
  const GiNaC::ex red = ArtinSchreierReduce(poly, x);
  if (red.is_zero()) {
    return 0;
  }
  return red.degree(x);
}

int H1Dimension(const GiNaC::ex& poly, const GiNaC::symbol& x) {
  const int swan = SwanAtInfinity(poly, x);
  return swan > 0 ? swan - 1 : 0;
}

}  // namespace primeparts::graph
