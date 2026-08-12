#pragma once

#include <ginac/ginac.h>

namespace primeparts::graph {

GiNaC::ex ReduceModTwo(const GiNaC::ex& c);

bool IsSquareModTwo(const GiNaC::ex& c);

GiNaC::ex SqrtModTwo(const GiNaC::ex& c);

GiNaC::ex ArtinSchreierReduce(const GiNaC::ex& poly, const GiNaC::symbol& x);

int SwanAtInfinity(const GiNaC::ex& poly, const GiNaC::symbol& x);

int H1Dimension(const GiNaC::ex& poly, const GiNaC::symbol& x);

}  // namespace primeparts::graph
