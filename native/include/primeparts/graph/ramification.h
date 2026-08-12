#pragma once

#include <ginac/ginac.h>

#include <cstdint>
#include <string>
#include <vector>

namespace primeparts::graph {

struct Place {
  int64_t e = 1;
  int64_t f = 1;
};

struct Local {
  int64_t degree = 0;
  std::vector<Place> places;
  bool ramified = false;
  bool tame = true;
  bool separable = true;
};

bool LocalAt(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
             uint64_t theta, uint64_t c, Local* out, std::string* error);

bool BranchLocus(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
                 uint64_t theta, std::vector<uint64_t>* out, std::string* error);

}  // namespace primeparts::graph
