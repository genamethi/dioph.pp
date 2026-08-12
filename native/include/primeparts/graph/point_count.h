#pragma once

#include <ginac/ginac.h>

#include <cstdint>
#include <string>
#include <vector>

namespace primeparts::graph {

struct FieldPoints {
  int p = 2;
  int d = 0;
  std::vector<int32_t> counts;

  int64_t size() const { return static_cast<int64_t>(counts.size()); }
};

struct TraceSpectrum {
  int p = 2;
  std::vector<int64_t> counts;
};

bool FiberCounts(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
                 uint64_t theta, FieldPoints* out, std::string* error);

bool Traces(const GiNaC::ex& poly, const GiNaC::symbol& x, int p, int d,
            uint64_t theta, TraceSpectrum* out, std::string* error);

int64_t WeilSum(const TraceSpectrum& s);

double WeilMagnitude(const TraceSpectrum& s);

bool Sum(const std::vector<GiNaC::ex>& polys, const GiNaC::symbol& x, int p,
         int d, uint64_t theta, FieldPoints* out, std::string* error);

int64_t Moment(const FieldPoints& a, int r);

int64_t Correlate(const FieldPoints& a, const FieldPoints& b);

int64_t TotalPoints(const FieldPoints& a);

}  // namespace primeparts::graph
