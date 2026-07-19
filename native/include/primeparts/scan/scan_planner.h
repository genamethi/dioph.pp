#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "primeparts/scan/scan_plan.h"

namespace iceberg {
class Expression;
class FileIO;
class FileScanTask;
class Schema;
struct TableMetadata;
}

namespace primeparts::scan {

bool PlanTableScan(const std::shared_ptr<iceberg::TableMetadata>& metadata,
                   const std::shared_ptr<iceberg::FileIO>& io,
                   const ScanPlanRequest& request, ScanPlan* out,
                   std::string* error);

bool RefineSplits(ScanPlan* plan, const std::shared_ptr<iceberg::FileIO>& io,
                  bool case_sensitive, std::string* error);

struct SplitSelection {
  iceberg::Split split;
  int64_t planned_rows = 0;
};

bool SelectSplits(const std::string& file_location, int64_t file_length,
                  const std::shared_ptr<iceberg::FileIO>& io,
                  const iceberg::Schema& schema,
                  const std::shared_ptr<iceberg::Expression>& residual,
                  bool case_sensitive, std::vector<SplitSelection>* splits,
                  bool* all_kept, std::string* error);

bool SortTasksByLowerBound(
    std::vector<std::shared_ptr<iceberg::FileScanTask>>* tasks,
    const iceberg::Schema& schema, const TableReadTraits::SortKey& key,
    std::string* error);

void DeriveKeyWindow(const std::shared_ptr<iceberg::Expression>& filter,
                     const std::string& key_name,
                     const std::shared_ptr<iceberg::PrimitiveType>& key_type,
                     std::optional<iceberg::Literal>* lo,
                     std::optional<iceberg::Literal>* hi);

}  // namespace primeparts::scan
