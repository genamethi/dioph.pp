#include "lcommon.h"

#include <cstdint>
#include <string>
#include <vector>

#include "primeparts/lua/graph/expr.h"
#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::BigInt;
using primeparts::graph::Expr;
using primeparts::graph::Poly;
using primeparts::graph::lua::CoeffTable;
using primeparts::graph::lua::PushBig;
using primeparts::graph::lua::Submodule;
using primeparts::lua::Fail;

void BindExpr(sol::table& expr) {
  expr.new_usertype<Expr>(
      "Expr", sol::no_constructor,
      "text", &Expr::Text,
      "expand", &Expr::Expand,
      "numeric", &Expr::IsNumeric,
      "symbols", [](const Expr& self, sol::this_state ts) {
        sol::state_view lua(ts);
        const std::vector<std::string> names = self.Symbols();
        sol::table out = lua.create_table(static_cast<int>(names.size()), 0);
        int idx = 1;
        for (const std::string& n : names) out[idx++] = n;
        return out;
      },
      "degree", &Expr::Degree,
      "subs", sol::overload(
          [](const Expr& self, const std::string& name, int64_t v) {
            return self.Subs(name, v);
          },
          [](const Expr& self, const std::string& name, const Expr& v) {
            return self.SubsExpr(name, v);
          }),
      "value", [](const Expr& self, sol::this_state ts) {
        std::string error;
        BigInt v = self.Value(&error);
        if (!error.empty()) Fail("Expr:value", error);
        return PushBig(sol::state_view(ts), v);
      },
      "poly", [](const Expr& self, const std::string& name) {
        Poly out;
        std::string error;
        if (!self.ToPoly(name, &out, &error)) Fail("Expr:poly", error);
        return out;
      },
      "he", [](const Expr& self, const std::string& name, sol::this_state ts) {
        Poly p;
        std::string error;
        if (!self.ToPoly(name, &p, &error)) Fail("Expr:he", error);
        return CoeffTable(sol::state_view(ts), p.Hermite());
      },
      sol::meta_function::addition, &Expr::Add,
      sol::meta_function::subtraction, &Expr::Sub,
      sol::meta_function::multiplication, &Expr::Mul,
      sol::meta_function::power_of, &Expr::Pow,
      sol::meta_function::to_string, &Expr::Text);
}

}  // namespace

extern "C" int luaopen_graph_expr(lua_State* L) {
  Submodule(L, "graph.poly", &luaopen_graph_poly);

  sol::state_view lua(L);
  sol::table expr = lua.create_table();
  expr.set_function("sym", &Expr::Symbol);
  expr.set_function("int", &Expr::Int);
  expr.set_function("pow2", &Expr::PowerOfTwo);
  expr.set_function("lift", &Expr::Lift);
  BindExpr(expr);
  expr.push();
  return 1;
}
