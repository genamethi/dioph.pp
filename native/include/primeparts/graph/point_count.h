#pragma once

#include <ginac/ginac.h>

#include <cstdint>
#include <string>
#include <vector>

namespace primeparts::graph {

struct FieldPoints {
  int d = 0;
  std::vector<int32_t> counts;

  int64_t size() const { return static_cast<int64_t>(counts.size()); }
};

bool FiberCounts(const GiNaC::ex& poly, const GiNaC::symbol& x, int d,
                 uint64_t theta, FieldPoints* out, std::string* error);

int64_t Correlate(const FieldPoints& a, const FieldPoints& b);

int64_t TotalPoints(const FieldPoints& a);

bool Sum(const std::vector<GiNaC::ex>& polys, const GiNaC::symbol& x, int d,
         uint64_t theta, FieldPoints* out, std::string* error);

int64_t Moment(const FieldPoints& a, int r);

}  // namespace primeparts::graph
