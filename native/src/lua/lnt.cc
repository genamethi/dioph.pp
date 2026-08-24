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
#include "primeparts/pp_covering.h"
#include "primeparts/pp_ppow.h"

namespace {

struct Part {
  int64_t m;
  int64_t n;
  int64_t q;
};

bool ResBit(const uint64_t* words, int modulus, uint64_t x) {
  const unsigned r = static_cast<unsigned>(x % static_cast<unsigned>(modulus));
  return (words[r >> 6] >> (r & 63)) & 1u;
}

bool Ppow(uint64_t x, uint64_t* base, int32_t* exp) {
  int32_t total = 1;

  for (int i = 0; i < PP_PPOW_NE; ++i) {
    const int n = pp_ppow_exp[i];
    for (;;) {
      if (n > pp_ppow_exp_at_bits[64 - __builtin_clzll(x)]) break;
      if (n == 2 && !((pp_ppow_square64 >> (x & 63)) & 1u)) break;
      if (!ResBit(pp_ppow_res_a[i], pp_ppow_mod_a[i], x)) break;
      if (!ResBit(pp_ppow_res_b[i], pp_ppow_mod_b[i], x)) break;

      ulong rem = 0;
      const ulong root = n_rootrem(&rem, static_cast<ulong>(x), static_cast<ulong>(n));
      if (rem != 0) break;
      if (root < PP_PPOW_MIN_BASE) return false;
      x = root;
      total *= static_cast<int32_t>(n);
    }
  }
  if (total == 1 || !n_is_prime(static_cast<ulong>(x))) return false;
  *base = x;
  *exp = total;
  return true;
}

bool CovExp(unsigned qi, uint64_t q, int32_t* exp) {
  const int n = pp_cov_pow_at_bits[qi][64 - __builtin_clzll(q)];

  if (n > 0 && pp_cov_pow[qi][n] == q) {
    *exp = static_cast<int32_t>(n);
    return true;
  }
  return false;
}

uint64_t Tile(uint64_t word, unsigned field, uint64_t lim) {
  uint64_t x = ((word >> (PP_COV_FIELD * field)) & PP_COV_FIELD_MASK) << 1;

  x |= x << 12;
  x |= x << 24;
  x |= x << 48;
  return x & lim;
}

void PartsOf(uint64_t p, std::vector<Part>* out) {
  const int max_m = 63 - __builtin_clzll(p);
  const uint64_t word = pp_cov_masks[p % PP_COV_MOD];
  uint64_t lim = max_m >= 63 ? ~UINT64_C(0)
                             : ((UINT64_C(1) << (max_m + 1)) - 1);
  lim &= ~UINT64_C(1);

  for (unsigned qi = 0; qi < PP_COV_NQ; ++qi) {
    uint64_t field = Tile(word, qi, lim);
    while (field != 0) {
      const int m = __builtin_ctzll(field);
      const uint64_t q = p - (UINT64_C(1) << m);
      int32_t exp = 0;
      field &= field - 1;
      if (q < 2) continue;
      if (CovExp(qi, q, &exp)) {
        out->push_back({m, exp, static_cast<int64_t>(pp_cov_q[qi])});
      }
    }
  }

  uint64_t rest = Tile(word, PP_COV_NQ, lim);
  while (rest != 0) {
    const int m = __builtin_ctzll(rest);
    const uint64_t q = p - (UINT64_C(1) << m);
    uint64_t base = 0;
    int32_t exp = 0;
    rest &= rest - 1;
    if (q < 2) continue;
    if (n_is_prime(static_cast<ulong>(q))) {
      out->push_back({m, 1, static_cast<int64_t>(q)});
    } else if (Ppow(q, &base, &exp)) {
      out->push_back({m, exp, static_cast<int64_t>(base)});
    }
  }
}

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
  if (p < 3) primeparts::lua::Fail("nt.genparts", "p must be at least 3");

  std::vector<Part> parts;
  PartsOf(input, &parts);
  const int64_t k = static_cast<int64_t>(parts.size());

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
