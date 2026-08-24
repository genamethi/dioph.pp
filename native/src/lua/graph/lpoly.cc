#include "lcommon.h"

#include <cstdint>
#include <string>

#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::Poly;
using primeparts::graph::lua::CoeffTable;
using primeparts::graph::lua::PushBig;
using primeparts::lua::Fail;

void BindPoly(sol::table& poly) {
  poly.new_usertype<Poly>(
      "Poly", sol::no_constructor,
      "degree", &Poly::Degree,
      "zero", &Poly::IsZero,
      "text", [](const Poly& self, sol::optional<std::string> var) {
        return self.Text(var.value_or("x"));
      },
      "pow", [](const Poly& self, int64_t e) {
        if (e < 0) Fail("Poly:pow", "exponent must be non-negative");
        return self.Pow(static_cast<uint64_t>(e));
      },
      "mono", [](const Poly& self, sol::this_state ts) {
        return CoeffTable(sol::state_view(ts), self.Monomial());
      },
      "he", [](const Poly& self, sol::this_state ts) {
        return CoeffTable(sol::state_view(ts), self.Hermite());
      },
      "eval", [](const Poly& self, int64_t x, sol::this_state ts) {
        return PushBig(sol::state_view(ts), self.Eval(x));
      },
      "mod", [](const Poly& self, const std::string& n) {
        std::string error;
        Poly out = self.ModCoeffs(n, &error);
        if (!error.empty()) Fail("Poly:mod", error);
        return out;
      },
      sol::meta_function::addition, &Poly::Add,
      sol::meta_function::subtraction, &Poly::Sub,
      sol::meta_function::multiplication, &Poly::Mul,
      sol::meta_function::equal_to, &Poly::Equals,
      sol::meta_function::to_string,
      [](const Poly& self) { return self.Text("x"); });
}

}  // namespace

extern "C" int luaopen_graph_poly(lua_State* L) {
  sol::state_view lua(L);
  sol::table poly = lua.create_table();
  poly.set_function("he", &Poly::He);
  poly.set_function("lift", &Poly::Lift);
  poly.set_function("x", &Poly::X);
  poly.set_function("const", &Poly::Constant);
  poly.set_function("pow2", &Poly::PowerOfTwo);
  BindPoly(poly);
  poly.push();
  return 1;
}
