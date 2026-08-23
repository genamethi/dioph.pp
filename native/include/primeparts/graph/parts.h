#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace primeparts::graph {

struct Part {
  int32_t m = 0;
  int32_t n = 0;
  int64_t q = 0;
};

class Oracle {
 public:
  virtual ~Oracle() = default;
  virtual bool Parts(int64_t p, std::vector<Part>* out, std::string* error) = 0;
  virtual const char* Name() const = 0;
  virtual int64_t Calls() const = 0;
};

bool DynamicParts(int64_t p, std::vector<Part>* out, std::string* error);

std::unique_ptr<Oracle> MakeDynamicOracle();

std::unique_ptr<Oracle> MakeCachedOracle(std::unique_ptr<Oracle> inner,
                                         std::size_t capacity);

}  // namespace primeparts::graph
