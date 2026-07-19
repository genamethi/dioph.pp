#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "primeparts/catalog/plan_status.h"
#include "primeparts/scan/scan_plan.h"

namespace primeparts::catalog {

class PlanStore {
 public:
  using Clock = std::chrono::steady_clock;
  using PlanFn = std::function<bool(scan::ScanPlan*, std::string*)>;

  struct Config {
    size_t batch_tasks = 64;
    std::chrono::seconds ttl{300};
  };

  struct Snapshot {
    PlanStatus status = PlanStatus::kSubmitted;
    std::string error;
    std::vector<scan::FileScanTask> tasks;
    std::vector<std::string> plan_tasks;
    std::shared_ptr<iceberg::Schema> table_schema;
  };

  PlanStore();
  explicit PlanStore(Config config);
  ~PlanStore();

  PlanStore(const PlanStore&) = delete;
  PlanStore& operator=(const PlanStore&) = delete;

  std::string Submit(PlanFn fn);

  bool Fetch(const std::string& plan_id, Snapshot* out);

  bool Cancel(const std::string& plan_id);

  bool FetchTasks(const std::string& plan_task,
                  std::vector<scan::FileScanTask>* out);

  size_t ExpireIdle(Clock::time_point now);

  size_t size() const;

 private:
  struct Entry {
    PlanStatus status = PlanStatus::kSubmitted;
    std::string error;
    std::vector<std::vector<scan::FileScanTask>> batches;
    std::vector<std::string> tokens;
    std::shared_ptr<iceberg::Schema> table_schema;
    Clock::time_point last_access;
    bool finished = false;
    std::atomic<bool> cancelled{false};
  };

  struct Worker {
    std::shared_ptr<Entry> entry;
    std::thread thread;
  };

  std::string NewTokenLocked();
  void DropTokensForLocked(const std::string& plan_id);
  void Publish(const std::shared_ptr<Entry>& entry, const std::string& plan_id,
               bool ok, scan::ScanPlan plan, std::string error);
  void ReapFinished();

  Config config_;
  mutable std::mutex mu_;
  std::mt19937_64 rng_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> plans_;
  std::unordered_map<std::string, std::pair<std::string, size_t>> tokens_;
  std::vector<Worker> workers_;
};

}  // namespace primeparts::catalog
