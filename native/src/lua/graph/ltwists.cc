#include "lcommon.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "primeparts/lua/lppconv.h"

namespace {

using primeparts::graph::Edge;
using primeparts::graph::TwistTable;
using primeparts::graph::lua::SetCall;
using primeparts::graph::lua::EdgeTable;
using primeparts::lua::Fail;
using primeparts::lua::IntOr;
using primeparts::lua::ReqInt;
using primeparts::lua::Spec;

std::shared_ptr<TwistTable> FromRows(const sol::table& rows,
                                     const sol::table& spec, const char* fn) {
  std::vector<Edge> parsed;
  int64_t hi = IntOr(spec, "hi", 0);
  for (std::size_t i = 1; i <= rows.size(); ++i) {
    sol::optional<sol::table> row = rows[i];
    if (!row) Fail(fn, "rows[" + std::to_string(i) + "] is not a table");
    const int64_t rp = (*row)["p"].get_or(int64_t{0});
    const int64_t rn = (*row)["n"].get_or(int64_t{0});
    if (rn < 2) continue;
    parsed.push_back(
        Edge{.m = static_cast<int32_t>((*row)["m"].get_or(int64_t{0})),
             .n = static_cast<int32_t>(rn),
             .q = (*row)["q"].get_or(int64_t{0}),
             .p = rp});
    if (rp > hi) hi = rp;
  }
  std::string error;
  std::unique_ptr<TwistTable> table =
      TwistTable::FromRows(std::move(parsed), hi, &error);
  if (!table) Fail(fn, error);
  return std::shared_ptr<TwistTable>(table.release());
}

std::shared_ptr<TwistTable> Build(sol::this_state ts,
                                  sol::optional<sol::table> arg) {
  static const char kFn[] = "graph.twists.build";
  const sol::table spec = Spec(ts, arg);

  sol::optional<sol::table> rows = spec["rows"];
  if (rows) return FromRows(*rows, spec, kFn);

  const int64_t hi = ReqInt(spec, "hi", kFn);
  std::string error;
  std::unique_ptr<TwistTable> table = TwistTable::Build(hi, &error);
  if (!table) Fail(kFn, error);
  return std::shared_ptr<TwistTable>(table.release());
}

void BindTable(sol::table& twists) {
  twists.new_usertype<TwistTable>(
      "TwistTable", sol::no_constructor,
      "hi", &TwistTable::Hi,
      "origin", [](const TwistTable& self) { return std::string(self.Origin()); },
      "count", [](const TwistTable& self) {
        return static_cast<int64_t>(self.Count());
      },
      "has", &TwistTable::Has,
      "at", [](const TwistTable& self, int64_t p, sol::this_state ts) {
        return EdgeTable(sol::state_view(ts), self.At(p));
      },
      "into", [](const TwistTable& self, int64_t q, sol::this_state ts) {
        return EdgeTable(sol::state_view(ts), self.Into(q));
      },
      "rows", [](const TwistTable& self, sol::optional<int64_t> limit,
                 sol::this_state ts) {
        const std::vector<Edge>& all = self.Rows();
        const std::size_t n =
            limit && *limit > 0
                ? std::min(all.size(), static_cast<std::size_t>(*limit))
                : all.size();
        return EdgeTable(sol::state_view(ts),
                         std::vector<Edge>(all.begin(), all.begin() + n));
      });
}

}  // namespace

extern "C" int luaopen_graph_twists(lua_State* L) {
  sol::state_view lua(L);
  sol::table twists = lua.create_table();
  twists.set_function("build", &Build);
  BindTable(twists);
  SetCall(lua, twists,
          sol::make_object(lua, [](sol::table, sol::this_state ts,
                                   sol::optional<sol::table> arg) {
            return Build(ts, arg);
          }));
  twists.push();
  return 1;
}
