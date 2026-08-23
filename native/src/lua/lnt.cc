#include "primeparts/lua/lnt.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <flint/ulong_extras.h>
#include <primecount.h>
#include <primesieve.h>
#include <sol/sol.hpp>

#include "primeparts/core.h"
#include "primeparts/lua/lppconv.h"

namespace {

struct Part {
  int64_t m;
  int64_t n;
  int64_t q;
};

int64_t Pi(int64_t n) { return primecount_pi(n); }

int64_t NthPrime(int64_t n) { return primecount_nth_prime(n); }

int64_t NextPrime(int64_t n) {
  return static_cast<int64_t>(primesieve_nth_prime(1, static_cast<uint64_t>(n)));
}

int64_t PrevPrime(int64_t n) {
  return static_cast<int64_t>(primesieve_nth_prime(-1, static_cast<uint64_t>(n)));
}

bool IsPrime(int64_t n) { return n_is_prime(static_cast<ulong>(n)) != 0; }

sol::variadic_results IsPrimePower(int64_t n, sol::this_state ts) {
  sol::state_view lua(ts);
  sol::variadic_results out;
  uint64_t base = 0;
  int32_t exponent = 0;

  if (pp_is_prime_power_u64(static_cast<uint64_t>(n), &base, &exponent) != PP_OK ||
      exponent <= 0) {
    out.push_back({lua, sol::in_place, sol::lua_nil});
    return out;
  }
  out.push_back({lua, sol::in_place, static_cast<int64_t>(base)});
  out.push_back({lua, sol::in_place, static_cast<int64_t>(exponent)});
  return out;
}

sol::table GenParts(int64_t p, sol::this_state ts) {
  const uint64_t input = static_cast<uint64_t>(p);
  pp_batch_result batch;
  pp_batch_result_init(&batch);

  const int rc = pp_process_prime_array(&input, 1, &batch);
  if (rc != PP_OK) {
    pp_batch_result_clear(&batch);
    primeparts::lua::Fail("nt.genparts", pp_status_message(rc));
  }

  std::vector<Part> parts;
  if (batch.prime_count > 0) {
    const uint64_t mask = batch.prime_flat_mask[0];
    for (int m = 0; m < 64; ++m) {
      if ((mask >> m) & 1u) {
        parts.push_back({m, 1, p - (int64_t{1} << m)});
      }
    }
  }
  for (std::size_t i = 0; i < batch.higher_count; ++i) {
    parts.push_back({batch.higher_m[i], batch.higher_n[i], batch.higher_q[i]});
  }
  const int64_t k = batch.prime_count > 0 ? batch.prime_k[0] : 0;
  pp_batch_result_clear(&batch);

  std::sort(parts.begin(), parts.end(),
            [](const Part& a, const Part& b) { return a.m < b.m; });

  sol::state_view lua(ts);
  sol::table out = lua.create_table();
  out["p"] = p;
  out["k"] = k;
  sol::table rows = lua.create_table(static_cast<int>(parts.size()), 0);
  int idx = 1;
  for (const Part& part : parts) {
    sol::table row = lua.create_table(0, 3);
    row["m"] = part.m;
    row["n"] = part.n;
    row["q"] = part.q;
    rows[idx++] = row;
  }
  out["partitions"] = rows;
  return out;
}

/* I'll implement this later.
 * it's just q powered up to n and m such that p = 2^m + q^n
 * doesn't surpass the sixty four bit limit.
 * Don't throw anything out. Just be smart.
sol::table InverseGen(int64_t q, sol::this_state ts) {


  sol::state_view lua(ts);
  sol::table out = lua.create_table();
  out["q"] = q;
  out["k"] = k;
  sol::table rows = lua.create_table(static_cast<int>(parts.size()), 0);
  int idx = 1;
  for (const Part& part : parts) {
    sol::table row = lua.create_table(0, 3);
    row["m"] = part.m;
    row["n"] = part.n;
    row["p"] = part.p;
    rows[idx++] = row;
  }
  out["partitions"] = rows;
  return out;

} */

}  // namespace

extern "C" int luaopen_nt(lua_State *L) {
  sol::state_view lua(L);
  sol::table nt = lua.create_table();
  nt.set_function("pi", &Pi);
  nt.set_function("nth_prime", &NthPrime);
  nt.set_function("next_prime", &NextPrime);
  nt.set_function("prev_prime", &PrevPrime);
  nt.set_function("is_prime", &IsPrime);
  nt.set_function("is_prime_power", &IsPrimePower);
  nt.set_function("genparts", &GenParts);
  //nt.set_function("invgp", &InverseGen);
  nt.push();
  return 1;
}
