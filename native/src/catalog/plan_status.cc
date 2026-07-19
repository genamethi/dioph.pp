#include "primeparts/catalog/plan_status.h"

namespace primeparts::catalog {

std::string_view PlanStatusName(PlanStatus status) {
  switch (status) {
    case PlanStatus::kSubmitted:
      return "submitted";
    case PlanStatus::kCompleted:
      return "completed";
    case PlanStatus::kFailed:
      return "failed";
    case PlanStatus::kCancelled:
      return "cancelled";
  }
  return "failed";
}

bool PlanStatusFromName(std::string_view name, PlanStatus* out) {
  if (name == "submitted") {
    *out = PlanStatus::kSubmitted;
  } else if (name == "completed") {
    *out = PlanStatus::kCompleted;
  } else if (name == "failed") {
    *out = PlanStatus::kFailed;
  } else if (name == "cancelled") {
    *out = PlanStatus::kCancelled;
  } else {
    return false;
  }
  return true;
}

}  // namespace primeparts::catalog
