#include "primeparts/higher_sweep.h"

#include <gtest/gtest.h>

#include <flint/ulong_extras.h>
#include <primesieve.h>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include "primeparts/core.h"

namespace {

struct Hit {
  uint64_t p;
  int32_t m;
  int32_t n;
  uint64_t q;

  auto Key() const { return std::tie(p, m, n, q); }
  bool operator==(const Hit& other) const { return Key() == other.Key(); }
  bool operator<(const Hit& other) const { return Key() < other.Key(); }
};

int FloorLog2(uint64_t x) { return 63 - __builtin_clzll(x); }

std::vector<Hit> Reference(uint64_t lo, uint64_t hi) {
  std::vector<Hit> out;
  primesieve_iterator it;

  primesieve_init(&it);
  primesieve_jump_to(&it, lo, hi);
  for (;;) {
    const uint64_t p = primesieve_next_prime(&it);
    if (p > hi) {
      break;
    }
    const int max_m = FloorLog2(p);
    const int killed_parity = (p % 3 == 2) ? 1 : 0;
    for (int m = 1; m <= max_m; ++m) {
      const uint64_t w = p - (uint64_t{1} << m);
      if (w < 2 || (m & 1) == killed_parity) {
        continue;
      }
      uint64_t base = 0;
      int32_t exponent = 0;
      EXPECT_EQ(pp_is_prime_power_u64(w, &base, &exponent), PP_OK);
      if (exponent >= 2) {
        out.push_back(Hit{p, static_cast<int32_t>(m), exponent, base});
      }
    }
  }
  primesieve_free_iterator(&it);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<Hit> Swept(uint64_t lo, uint64_t hi) {
  pp_higher_table table;
  std::vector<Hit> out;

  pp_higher_table_init(&table);
  EXPECT_EQ(pp_higher_sweep(&table, lo, hi), PP_OK);
  for (size_t i = 0; i < table.count; ++i) {
    const pp_higher_hit& h = table.hits[i];
    out.push_back(Hit{static_cast<uint64_t>(h.p), h.m, h.n,
                      static_cast<uint64_t>(h.q)});
  }
  pp_higher_table_clear(&table);
  std::sort(out.begin(), out.end());
  return out;
}

void ExpectMatchesReference(uint64_t lo, uint64_t hi) {
  const std::vector<Hit> expected = Reference(lo, hi);
  const std::vector<Hit> actual = Swept(lo, hi);

  ASSERT_EQ(actual.size(), expected.size())
      << "window [" << lo << ", " << hi << "]";
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i], expected[i])
        << "index " << i << " in window [" << lo << ", " << hi << "]";
  }
}

TEST(HigherSweepTest, MatchesReferenceOverSmallWindow) {
  ExpectMatchesReference(3, 200000);
}

TEST(HigherSweepTest, MatchesReferenceNearOneBillion) {
  ExpectMatchesReference(1000000000, 1002000000);
}

TEST(HigherSweepTest, MatchesReferenceAtFortyBits) {
  ExpectMatchesReference(uint64_t{1} << 40, (uint64_t{1} << 40) + 4000000);
}

TEST(HigherSweepTest, MatchesReferenceAtFiftyBits) {
  ExpectMatchesReference(uint64_t{1} << 50, (uint64_t{1} << 50) + 20000000);
}

TEST(HigherSweepTest, FindsTheTwentyNineAnchor) {
  const std::vector<Hit> hits = Swept(3, 100);

  ASSERT_FALSE(hits.empty());
  EXPECT_NE(std::find(hits.begin(), hits.end(), Hit{29, 2, 2, 5}), hits.end());
}

TEST(HigherSweepTest, ExcludesBasesDivisibleByThree) {
  for (const Hit& h : Swept(3, 2000000)) {
    EXPECT_NE(h.q % 3, 0u) << "p=" << h.p;
    EXPECT_GE(h.q, 5u);
  }
}

TEST(HigherSweepTest, HitsSatisfyThePartitionIdentity) {
  for (const Hit& h : Swept(3, 2000000)) {
    __uint128_t power = 1;
    for (int32_t i = 0; i < h.n; ++i) {
      power *= h.q;
    }
    EXPECT_GE(h.n, 2);
    EXPECT_EQ(power + (__uint128_t{1} << h.m), __uint128_t{h.p});
    EXPECT_TRUE(n_is_prime(h.q));
    EXPECT_TRUE(n_is_prime(h.p));
  }
}

TEST(HigherSweepTest, AdjacentWindowsPartitionTheResult) {
  const uint64_t lo = 500000000;
  const uint64_t mid = 501000000;
  const uint64_t hi = 502000000;

  std::vector<Hit> split = Swept(lo, mid);
  const std::vector<Hit> upper = Swept(mid + 1, hi);
  split.insert(split.end(), upper.begin(), upper.end());
  std::sort(split.begin(), split.end());

  EXPECT_EQ(split, Swept(lo, hi));
}

TEST(HigherSweepTest, BoundsAreInclusive) {
  const std::vector<Hit> hits = Swept(3, 100000);

  ASSERT_FALSE(hits.empty());
  const Hit first = hits.front();
  const Hit last = hits.back();

  EXPECT_EQ(Swept(first.p, first.p), std::vector<Hit>{first});
  EXPECT_EQ(Swept(last.p, last.p), std::vector<Hit>{last});
  EXPECT_TRUE(Swept(first.p + 1, last.p - 1).empty() ||
              Swept(first.p + 1, last.p - 1).front().p > first.p);
}

TEST(HigherSweepTest, EmptyWindowsYieldNothing) {
  EXPECT_TRUE(Swept(3, 3).empty());
  EXPECT_TRUE(Swept(100, 10).empty());
}

TEST(HigherSweepTest, CursorConsumesInPositionOrder) {
  pp_higher_table table;

  pp_higher_table_init(&table);
  ASSERT_EQ(pp_higher_sweep(&table, 3, 2000000), PP_OK);
  ASSERT_GT(table.count, 0u);

  std::vector<pp_higher_hit> ordered(table.hits, table.hits + table.count);
  size_t taken = 0;
  for (const pp_higher_hit& want : ordered) {
    pp_higher_seek(&table, static_cast<uint64_t>(want.p));
    EXPECT_EQ(pp_higher_take(&table, static_cast<uint64_t>(want.p), want.m - 1),
              nullptr);
    const pp_higher_hit* got =
        pp_higher_take(&table, static_cast<uint64_t>(want.p), want.m);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->q, want.q);
    EXPECT_EQ(got->n, want.n);
    ++taken;
  }
  EXPECT_EQ(taken, ordered.size());
  EXPECT_EQ(table.cursor, table.count);
  pp_higher_table_clear(&table);
}

}  // namespace
