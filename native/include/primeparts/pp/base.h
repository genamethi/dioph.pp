#pragma once

#include <ginac/ginac.h>

#include <cstdint>

namespace primeparts::pp {

const GiNaC::symbol& Base();

GiNaC::ex FromMask(uint64_t mask);

bool ToMask(const GiNaC::ex& c, uint64_t* mask);

GiNaC::ex Evaluate(const GiNaC::ex& e);

}  // namespace primeparts::pp
