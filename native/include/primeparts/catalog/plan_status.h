#pragma once

#include <string_view>

namespace primeparts::catalog {

enum class PlanStatus { kSubmitted, kCompleted, kFailed, kCancelled };

std::string_view PlanStatusName(PlanStatus status);

bool PlanStatusFromName(std::string_view name, PlanStatus* out);

}  // namespace primeparts::catalog
