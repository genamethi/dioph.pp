#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/table_identifier.h"
#include "iceberg/type_fwd.h"
#include "primeparts/catalog/plan_status.h"
#include "primeparts/scan/scan_plan.h"

namespace iceberg {
class FileScanTask;
struct TableMetadata;
}

namespace primeparts::catalog {

enum class ScanPlanningMode { kClient, kServer };

bool FetchScanPlanningMode(const std::string& rest_uri,
                           const iceberg::Namespace& ns,
                           const std::string& table, ScanPlanningMode* out,
                           std::string* error);

struct PlanSubmission {
  std::string plan_id;
  PlanStatus status = PlanStatus::kSubmitted;
};

struct PlanResult {
  PlanStatus status = PlanStatus::kSubmitted;
  std::string failure;
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  std::vector<std::string> plan_tasks;
};

struct PlanPollOptions {
  std::chrono::milliseconds interval{25};
  std::chrono::milliseconds timeout{60000};
};

bool SubmitTableScan(const std::string& rest_uri, const iceberg::Namespace& ns,
                     const std::string& table,
                     const scan::ScanPlanRequest& request,
                     const iceberg::TableMetadata& metadata,
                     PlanSubmission* out, std::string* error);

bool FetchPlanningResult(const std::string& rest_uri,
                         const iceberg::Namespace& ns, const std::string& table,
                         const std::string& plan_id,
                         const iceberg::TableMetadata& metadata,
                         PlanResult* out, std::string* error);

bool CancelPlanning(const std::string& rest_uri, const iceberg::Namespace& ns,
                    const std::string& table, const std::string& plan_id,
                    std::string* error);

bool FetchScanTasks(const std::string& rest_uri, const iceberg::Namespace& ns,
                    const std::string& table, const std::string& plan_task,
                    const iceberg::TableMetadata& metadata,
                    std::vector<std::shared_ptr<iceberg::FileScanTask>>* out,
                    std::string* error);

bool PlanScanOnServer(const std::string& rest_uri, const iceberg::Namespace& ns,
                      const std::string& table,
                      const scan::ScanPlanRequest& request,
                      const iceberg::TableMetadata& metadata,
                      const PlanPollOptions& poll,
                      std::vector<std::shared_ptr<iceberg::FileScanTask>>* out,
                      std::string* error);

}  // namespace primeparts::catalog
