#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iceberg/expression/literal.h"
#include "iceberg/file_reader.h"
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
  std::optional<int64_t> min_rows_requested;
  bool case_sensitive = true;
  bool use_snapshot_schema = false;
  std::optional<int64_t> start_snapshot_id;
  std::optional<int64_t> end_snapshot_id;
  std::vector<std::string> stats_fields;
};

struct FileScanTask {
  std::shared_ptr<iceberg::FileScanTask> inner;
  std::optional<iceberg::Split> split;
  int64_t planned_rows = 0;
};

struct ScanPlan {
  std::shared_ptr<iceberg::Schema> table_schema;
  std::shared_ptr<iceberg::Schema> projected_schema;
  TableReadTraits traits;
  std::vector<FileScanTask> tasks;
  int64_t planned_rows = 0;
  std::optional<iceberg::Literal> key_lo;
  std::optional<iceberg::Literal> key_hi;
  std::shared_ptr<iceberg::Expression> residual;
};

}  // namespace primeparts::scan
