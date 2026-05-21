#include "primeparts/primitive_factors.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool ContainsFactor(const std::vector<primeparts::PrimitiveFactor>& factors,
                    uint64_t q) {
  for (const auto& f : factors) {
    if (f.q == q) return true;
  }
  return false;
}

}  // namespace

int main() {
  using primeparts::BuildMersenneHelper;
  using primeparts::IsBackboneCovered;
  using primeparts::IsPrimitiveFactor;
  using primeparts::PrimitiveFactorsForTerm;

  auto helper = BuildMersenneHelper(12);
  if (!Check(helper.ord2_by_q.at(3) == 2, "ord_3(2) should be 2")) {
    return 1;
  }
  if (!Check(helper.ord2_by_q.at(7) == 3, "ord_7(2) should be 3")) {
    return 1;
  }
  if (!Check(helper.ord2_by_q.at(5) == 4, "ord_5(2) should be 4")) {
    return 1;
  }
  if (!Check(helper.ord2_by_q.at(13) == 12, "ord_13(2) should be 12")) {
    return 1;
  }

  if (!Check(IsBackboneCovered(11, 1), "11 - 2^1 should be covered by q=3")) {
    return 1;
  }
  if (!Check(IsBackboneCovered(11, 2), "11 - 2^2 should be covered by q=7")) {
    return 1;
  }
  if (!Check(!IsBackboneCovered(31, 1),
             "31 - 2^1 should not be covered by {3,5,7}")) {
    return 1;
  }

  auto first = PrimitiveFactorsForTerm(31, 1, helper);
  if (!Check(ContainsFactor(first, 29),
             "29 should be primitive for a_1 = 31 - 2")) {
    return 1;
  }

  if (!Check(IsPrimitiveFactor(211, 4, 13, helper),
             "13 should be primitive at m=4 when ord_13(2)=12")) {
    return 1;
  }
  if (!Check(!IsPrimitiveFactor(8231, 13, 13, helper),
             "13 should not be primitive at m=13 when ord_13(2)=12")) {
    return 1;
  }

  auto repeated = PrimitiveFactorsForTerm(8231, 13, helper);
  if (!Check(!ContainsFactor(repeated, 29),
             "29 should not appear in an unrelated repeated-factor check")) {
    return 1;
  }
  if (!Check(!ContainsFactor(repeated, 13),
             "13 should not be primitive again at a_13 = 8231 - 2^13")) {
    return 1;
  }

  auto composite = PrimitiveFactorsForTerm(211, 4, helper);
  if (!Check(ContainsFactor(composite, 13),
             "13 should be a primitive factor of 211 - 2^4 = 195")) {
    return 1;
  }
  if (!Check(!ContainsFactor(composite, 3) && !ContainsFactor(composite, 5),
             "backbone factors should not be reported as primitive output")) {
    return 1;
  }

  return 0;
}
