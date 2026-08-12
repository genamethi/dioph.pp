#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace primeparts::graph {

struct Cyclotomic {
  int64_t order = 1;
  std::vector<int64_t> counts;
};

double Magnitude(const Cyclotomic& c);

int64_t Total(const Cyclotomic& c);

bool GaussSum(int p, int d, int64_t a, Cyclotomic* out, std::string* error);

bool JacobiSum(int p, int d, int64_t a, int64_t b, Cyclotomic* out,
               std::string* error);

}  // namespace primeparts::graph
