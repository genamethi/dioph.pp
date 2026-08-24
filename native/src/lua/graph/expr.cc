#include "primeparts/lua/graph/expr.h"

#include <ginac/ginac.h>

#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace primeparts::graph {

namespace {

GiNaC::symbol NamedSymbol(const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, GiNaC::symbol> table;
  std::lock_guard<std::mutex> guard(mu);
  const auto it = table.find(name);
  if (it != table.end()) return it->second;
  return table.emplace(name, GiNaC::symbol(name)).first->second;
}

bool AsLong(const GiNaC::ex& e, long* out) {
  if (!GiNaC::is_a<GiNaC::numeric>(e)) return false;
  const GiNaC::numeric n = GiNaC::ex_to<GiNaC::numeric>(e);
  if (!n.is_integer()) return false;
  static const GiNaC::numeric lo(std::numeric_limits<long>::min());
  static const GiNaC::numeric hi(std::numeric_limits<long>::max());
  if (n < lo || n > hi) return false;
  *out = n.to_long();
  return true;
}

std::string ExText(const GiNaC::ex& e) {
  std::ostringstream os;
  os << e;
  return os.str();
}

}  // namespace

struct Expr::Impl {
  GiNaC::ex e;
};

Expr::Expr() : impl_(std::make_unique<Impl>()) { impl_->e = 0; }
Expr::~Expr() = default;
Expr::Expr(Expr&&) noexcept = default;
Expr& Expr::operator=(Expr&&) noexcept = default;

Expr::Expr(const Expr& other) : impl_(std::make_unique<Impl>()) {
  impl_->e = other.impl_->e;
}

Expr& Expr::operator=(const Expr& other) {
  if (this != &other) impl_->e = other.impl_->e;
  return *this;
}

Expr Expr::Symbol(const std::string& name) {
  Expr out;
  out.impl_->e = NamedSymbol(name);
  return out;
}

Expr Expr::Int(int64_t value) {
  Expr out;
  out.impl_->e = GiNaC::numeric(static_cast<long>(value));
  return out;
}

Expr Expr::PowerOfTwo(int32_t m) {
  Expr out;
  out.impl_->e = GiNaC::pow(GiNaC::numeric(2), GiNaC::numeric(m));
  return out;
}

Expr Expr::Lift(int64_t v, const std::string& name) {
  const GiNaC::symbol t = NamedSymbol(name);
  Expr out;
  out.impl_->e = 0;
  for (int m = 0; m < 63; ++m) {
    if (((v >> m) & 1) == 0) continue;
    out.impl_->e = out.impl_->e + GiNaC::pow(t, GiNaC::numeric(m));
  }
  return out;
}

Expr Expr::Add(const Expr& other) const {
  Expr out;
  out.impl_->e = impl_->e + other.impl_->e;
  return out;
}

Expr Expr::Sub(const Expr& other) const {
  Expr out;
  out.impl_->e = impl_->e - other.impl_->e;
  return out;
}

Expr Expr::Mul(const Expr& other) const {
  Expr out;
  out.impl_->e = impl_->e * other.impl_->e;
  return out;
}

Expr Expr::Pow(int64_t e) const {
  Expr out;
  out.impl_->e = GiNaC::pow(impl_->e, GiNaC::numeric(static_cast<long>(e)));
  return out;
}

Expr Expr::Expand() const {
  Expr out;
  out.impl_->e = GiNaC::expand(impl_->e);
  return out;
}

Expr Expr::Subs(const std::string& name, int64_t value) const {
  Expr out;
  out.impl_->e =
      impl_->e.subs(NamedSymbol(name) == GiNaC::numeric(static_cast<long>(value)));
  return out;
}

Expr Expr::SubsExpr(const std::string& name, const Expr& value) const {
  Expr out;
  out.impl_->e = impl_->e.subs(NamedSymbol(name) == value.impl_->e);
  return out;
}

bool Expr::IsNumeric() const {
  return GiNaC::is_a<GiNaC::numeric>(impl_->e.expand());
}

BigInt Expr::Value(std::string* error) const {
  BigInt out;
  const GiNaC::ex v = impl_->e.expand();
  if (!GiNaC::is_a<GiNaC::numeric>(v)) {
    *error = "expression is not a number; substitute its symbols first";
    return out;
  }
  long fits = 0;
  if (AsLong(v, &fits)) {
    out.value = fits;
    out.fits = true;
    return out;
  }
  out.fits = false;
  out.text = ExText(v);
  return out;
}

bool Expr::EqualsInt(int64_t target) const {
  const GiNaC::ex v = impl_->e.expand();
  if (!GiNaC::is_a<GiNaC::numeric>(v)) return false;
  return v == GiNaC::numeric(static_cast<long>(target));
}

std::vector<std::string> Expr::Symbols() const {
  std::vector<std::string> out;
  for (auto it = impl_->e.preorder_begin(); it != impl_->e.preorder_end(); ++it) {
    if (GiNaC::is_a<GiNaC::symbol>(*it)) {
      const std::string name = GiNaC::ex_to<GiNaC::symbol>(*it).get_name();
      bool seen = false;
      for (const std::string& s : out) seen = seen || s == name;
      if (!seen) out.push_back(name);
    }
  }
  return out;
}

int64_t Expr::Degree(const std::string& name) const {
  return impl_->e.expand().degree(NamedSymbol(name));
}

bool Expr::ToPoly(const std::string& name, Poly* out, std::string* error) const {
  const GiNaC::symbol s = NamedSymbol(name);
  const GiNaC::ex e = impl_->e.expand();
  if (!e.is_polynomial(s)) {
    *error = "expression is not polynomial in " + name;
    return false;
  }
  const int deg = e.degree(s);
  Poly acc;
  for (int i = 0; i <= deg; ++i) {
    const GiNaC::ex c = e.coeff(s, i);
    if (!GiNaC::is_a<GiNaC::numeric>(c)) {
      *error = "coefficient of " + name + "^" + std::to_string(i) +
               " is not a number; substitute the other symbols first";
      return false;
    }
    if (c.is_zero()) continue;
    long fits = 0;
    if (AsLong(c, &fits)) {
      acc = acc.Add(Poly::Term(fits, i));
      continue;
    }
    Poly term;
    std::string perr;
    if (!PolyFromDecimal(ExText(c), i, &term, &perr)) {
      *error = perr;
      return false;
    }
    acc = acc.Add(term);
  }
  *out = std::move(acc);
  return true;
}

int64_t Expr::LowDegree(const std::string& name) const {
  return impl_->e.expand().ldegree(NamedSymbol(name));
}

Expr Expr::Coeff(const std::string& name, int64_t power) const {
  Expr out;
  out.impl_->e = impl_->e.expand().coeff(NamedSymbol(name),
                                         static_cast<int>(power));
  return out;
}

std::string Expr::Head() const {
  const GiNaC::ex& e = impl_->e;
  if (GiNaC::is_a<GiNaC::numeric>(e)) return "numeric";
  if (GiNaC::is_a<GiNaC::symbol>(e)) return "symbol";
  if (GiNaC::is_a<GiNaC::add>(e)) return "add";
  if (GiNaC::is_a<GiNaC::mul>(e)) return "mul";
  if (GiNaC::is_a<GiNaC::power>(e)) return "power";
  if (GiNaC::is_a<GiNaC::function>(e)) return "function";
  return "other";
}

int64_t Expr::Arity() const { return impl_->e.nops(); }

Expr Expr::Op(int64_t i) const {
  Expr out;
  if (i < 0 || static_cast<std::size_t>(i) >= impl_->e.nops()) {
    out.impl_->e = 0;
    return out;
  }
  out.impl_->e = impl_->e.op(static_cast<std::size_t>(i));
  return out;
}

std::string Expr::Text() const { return ExText(impl_->e); }

}  // namespace primeparts::graph
