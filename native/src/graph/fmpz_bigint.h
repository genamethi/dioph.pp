#pragma once

#include <cstdlib>

#include <flint/fmpz.h>

#include "primeparts/graph/bigint.h"

namespace primeparts::graph {

inline BigInt ToBigInt(const fmpz_t z) {
  BigInt out;
  if (fmpz_fits_si(z)) {
    out.fits = true;
    out.value = fmpz_get_si(z);
    return out;
  }
  char* text = fmpz_get_str(nullptr, 10, z);
  out.fits = false;
  out.text = text;
  std::free(text);
  return out;
}

}  // namespace primeparts::graph
