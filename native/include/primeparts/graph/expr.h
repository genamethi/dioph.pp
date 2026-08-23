#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/graph/bigint.h"
#include "primeparts/graph/poly.h"

namespace primeparts::graph {

class Expr {
 public:
  Expr();
  ~Expr();
  Expr(const Expr& other);
  Expr& operator=(const Expr& other);
  Expr(Expr&& other) noexcept;
  Expr& operator=(Expr&& other) noexcept;

  static Expr Symbol(const std::string& name);
  static Expr Int(int64_t value);
  static Expr PowerOfTwo(int32_t m);

  Expr Add(const Expr& other) const;
  Expr Sub(const Expr& other) const;
  Expr Mul(const Expr& other) const;
  Expr Pow(int64_t e) const;

  Expr Expand() const;
  Expr Subs(const std::string& name, int64_t value) const;
  Expr SubsExpr(const std::string& name, const Expr& value) const;

  bool IsNumeric() const;
  BigInt Value(std::string* error) const;
  bool EqualsInt(int64_t target) const;

  std::vector<std::string> Symbols() const;
  int64_t Degree(const std::string& name) const;

  bool ToPoly(const std::string& name, Poly* out, std::string* error) const;

  std::string Text() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace primeparts::graph
