#pragma once

#include <ginac/ginac.h>

#include <cstdint>
#include <vector>

namespace primeparts::graph {

const GiNaC::symbol& TSymbol();

GiNaC::ex MaskToT(uint64_t mask);
bool TToMask(const GiNaC::ex& c, uint64_t* mask);
GiNaC::ex EvalAtTwo(const GiNaC::ex& e);

struct Step {
  int32_t m = 0;
  int32_t n = 1;
};

struct Word {
  int64_t root = 0;
  int64_t target = 0;
  std::vector<Step> steps;
};

int64_t Degree(const Word& w);

std::vector<GiNaC::symbol> ChainSymbols(const Word& w);

GiNaC::ex StepGenerator(const Step& s, const GiNaC::ex& y_in,
                        const GiNaC::ex& y_out);

std::vector<GiNaC::ex> Uncollapsed(const Word& w,
                                   const std::vector<GiNaC::symbol>& y);

std::vector<GiNaC::ex> Graded(const Word& w, const GiNaC::symbol& x);

GiNaC::ex Composed(const Word& w, const GiNaC::symbol& x);

GiNaC::ex FiberProduct(const Word& a, const Word& b, const GiNaC::symbol& y,
                       const GiNaC::symbol& y_other);

bool ClosesAtTwo(const Word& w);

}  // namespace primeparts::graph
