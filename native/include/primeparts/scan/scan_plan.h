#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "primeparts/scan/table_traits.h"

namespace iceberg {
class Expression;
class FileScanTask;
class Schema;
}

namespace primeparts::scan {

struct ScanPlanRequest {
  std::optional<int64_t> snapshot_id;
  std::vector<std::string> select;
  std::shared_ptr<iceberg::Expression> filter;
  bool case_sensitive = true;
  std::optional<int64_t> start_snapshot_id;
  std::optional<int64_t> end_snapshot_id;
  std::vector<std::string> stats_fields;
};

struct FileScanTask {
  std::shared_ptr<iceberg::FileScanTask> inner;
  std::vector<int32_t> row_groups;
  int64_t planned_rows = 0;
};

struct ScanPlan {
  std::shared_ptr<iceberg::Schema> projected_schema;
  TableReadTraits traits;
  std::vector<FileScanTask> tasks;
  int64_t planned_rows = 0;
};

}  // namespace primeparts::scan
