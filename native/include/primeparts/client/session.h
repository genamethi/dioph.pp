#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "primeparts/catalog/rest_scan_plan.h"
#include "primeparts/scan/scan_plan.h"

namespace arrow {
class RecordBatch;
}

namespace iceberg {
class Catalog;
class FileIO;
struct TableMetadata;
}

namespace primeparts::client {

struct SessionOptions {
  std::string rest_uri;
  std::string warehouse;
  std::string ns;
  int scan_threads = 1;
  int64_t read_batch_size = 0;
  catalog::PlanPollOptions poll;
};

class TableHandle {
 public:
  const std::string& name() const;
  const std::shared_ptr<iceberg::TableMetadata>& metadata() const;
  catalog::ScanPlanningMode planning_mode() const;

 private:
  friend class Session;
  struct State;
  std::shared_ptr<const State> state_;
};

class ScanStream {
 public:
  ~ScanStream();
  ScanStream(const ScanStream&) = delete;
  ScanStream& operator=(const ScanStream&) = delete;

  bool Next(std::shared_ptr<arrow::RecordBatch>* out, std::string* error);

  int64_t planned_rows() const;
  int64_t file_count() const;
  int shard_count() const;
  catalog::ScanPlanningMode planned_via() const;

 private:
  friend class Session;
  struct Impl;
  explicit ScanStream(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class Session {
 public:
  static std::unique_ptr<Session> Open(const SessionOptions& options,
                                       std::string* error);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool LoadTable(const std::string& table, TableHandle* out,
                 std::string* error);

  std::unique_ptr<ScanStream> Scan(const TableHandle& table,
                                   const scan::ScanPlanRequest& request,
                                   std::string* error);

  bool PlanFiles(const TableHandle& table, const scan::ScanPlanRequest& request,
                 std::vector<std::string>* paths, std::string* error);

  bool FieldUpperBound(const TableHandle& table, const std::string& field,
                       int64_t* out, bool* present, std::string* error);

  const iceberg::Namespace& ns() const;
  const std::string& rest_uri() const;
  const std::shared_ptr<iceberg::Catalog>& catalog() const;
  const std::shared_ptr<iceberg::FileIO>& io() const;

 private:
  bool Plan(const TableHandle& table, const scan::ScanPlanRequest& request,
            scan::ScanPlan* out, std::string* error);

  struct Impl;
  explicit Session(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace primeparts::client
