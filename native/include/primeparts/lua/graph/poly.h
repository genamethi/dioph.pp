#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/lua/graph/bigint.h"

namespace primeparts::graph {

struct Coeff {
  int64_t index = 0;
  BigInt value;
};

class Poly {
 public:
  Poly();
  ~Poly();
  Poly(const Poly& other);
  Poly& operator=(const Poly& other);
  Poly(Poly&& other) noexcept;
  Poly& operator=(Poly&& other) noexcept;

  static Poly X();
  static Poly Constant(int64_t c);
  static Poly PowerOfTwo(int32_t m);
  static Poly He(int64_t n);
  static Poly Lift(int64_t v);
  static Poly Term(int64_t coeff, int64_t exponent);

  int64_t Degree() const;
  bool IsZero() const;

  Poly Add(const Poly& other) const;
  Poly Sub(const Poly& other) const;
  Poly Mul(const Poly& other) const;
  Poly Pow(uint64_t e) const;
  Poly ModCoeffs(const std::string& modulus, std::string* error) const;

  bool Equals(const Poly& other) const;

  BigInt Eval(int64_t x) const;
  bool EvalEquals(int64_t x, int64_t target) const;

  std::vector<Coeff> Monomial() const;
  std::vector<Coeff> Hermite() const;

  std::string Text(const std::string& var) const;

 private:
  struct Impl;
  friend bool PolyFromDecimal(const std::string&, int64_t, Poly*,
                              std::string*);
  std::unique_ptr<Impl> impl_;
};


bool PolyFromDecimal(const std::string& decimal, int64_t exponent, Poly* out,
                     std::string* error);

}  // namespace primeparts::graph
