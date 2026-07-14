#pragma once

#include <cstdint>
#include <memory>
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

bool SelectRowGroups(const std::string& file_path,
                     const iceberg::Schema& schema,
                     const std::shared_ptr<iceberg::Expression>& residual,
                     bool case_sensitive, std::vector<int32_t>* row_groups,
                     int64_t* planned_rows, std::string* error);

bool SortTasksByLowerBound(
    std::vector<std::shared_ptr<iceberg::FileScanTask>>* tasks,
    const iceberg::Schema& schema, const TableReadTraits::SortKey& key,
    std::string* error);

}  // namespace primeparts::scan
