#include "primeparts/catalog/plan_store.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

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

PlanStore::PlanStore() : PlanStore(Config()) {}

PlanStore::PlanStore(Config config)
    : config_(config), rng_(std::random_device{}()) {
  if (config_.batch_tasks == 0) config_.batch_tasks = 1;
}

PlanStore::~PlanStore() {
  std::vector<Worker> workers;
  {
    std::lock_guard<std::mutex> lock(mu_);
    workers = std::move(workers_);
    workers_.clear();
  }
  for (auto& worker : workers) {
    if (worker.thread.joinable()) worker.thread.join();
  }
}

std::string PlanStore::NewTokenLocked() {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << rng_()
      << std::setw(16) << rng_();
  return out.str();
}

void PlanStore::DropTokensForLocked(const std::string& plan_id) {
  for (auto it = tokens_.begin(); it != tokens_.end();) {
    it = it->second.first == plan_id ? tokens_.erase(it) : std::next(it);
  }
}

void PlanStore::ReapFinished() {
  std::vector<std::thread> done;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = workers_.begin(); it != workers_.end();) {
      if (it->entry->finished) {
        done.push_back(std::move(it->thread));
        it = workers_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& thread : done) {
    if (thread.joinable()) thread.join();
  }
}

void PlanStore::Publish(const std::shared_ptr<Entry>& entry,
                        const std::string& plan_id, bool ok,
                        scan::ScanPlan plan, std::string error) {
  std::lock_guard<std::mutex> lock(mu_);
  entry->finished = true;
  entry->last_access = Clock::now();

  if (entry->cancelled.load()) {
    entry->status = PlanStatus::kCancelled;
    return;
  }
  if (!ok) {
    entry->status = PlanStatus::kFailed;
    entry->error = std::move(error);
    return;
  }

  entry->status = PlanStatus::kCompleted;
  entry->table_schema = plan.table_schema;

  for (size_t start = 0; start < plan.tasks.size();
       start += config_.batch_tasks) {
    const size_t end = std::min(start + config_.batch_tasks, plan.tasks.size());
    entry->batches.emplace_back(
        std::make_move_iterator(plan.tasks.begin() + start),
        std::make_move_iterator(plan.tasks.begin() + end));
  }
  if (entry->batches.empty()) entry->batches.emplace_back();

  for (size_t batch = 1; batch < entry->batches.size(); ++batch) {
    auto token = NewTokenLocked();
    tokens_.emplace(token, std::make_pair(plan_id, batch));
    entry->tokens.push_back(std::move(token));
  }
}

std::string PlanStore::Submit(PlanFn fn) {
  ReapFinished();

  auto entry = std::make_shared<Entry>();
  std::string plan_id;
  {
    std::lock_guard<std::mutex> lock(mu_);
    plan_id = NewTokenLocked();
    entry->last_access = Clock::now();
    plans_.emplace(plan_id, entry);
  }

  std::thread thread([this, entry, plan_id, fn = std::move(fn)]() {
    scan::ScanPlan plan;
    std::string error;
    const bool ok = fn(&plan, &error);
    Publish(entry, plan_id, ok, std::move(plan), std::move(error));
  });

  {
    std::lock_guard<std::mutex> lock(mu_);
    workers_.push_back(Worker{entry, std::move(thread)});
  }
  return plan_id;
}

bool PlanStore::Fetch(const std::string& plan_id, Snapshot* out) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = plans_.find(plan_id);
  if (it == plans_.end()) return false;

  auto& entry = *it->second;
  entry.last_access = Clock::now();

  out->status = entry.status;
  out->error = entry.error;
  out->table_schema = entry.table_schema;
  out->tasks.clear();
  out->plan_tasks.clear();

  if (entry.status == PlanStatus::kCompleted) {
    if (!entry.batches.empty()) out->tasks = entry.batches.front();
    out->plan_tasks = entry.tokens;
  }
  return true;
}

bool PlanStore::Cancel(const std::string& plan_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = plans_.find(plan_id);
  if (it == plans_.end()) return false;

  auto& entry = *it->second;
  if (entry.status != PlanStatus::kSubmitted) return true;

  entry.cancelled.store(true);
  entry.status = PlanStatus::kCancelled;
  entry.last_access = Clock::now();
  entry.batches.clear();
  entry.tokens.clear();
  DropTokensForLocked(plan_id);
  return true;
}

bool PlanStore::FetchTasks(const std::string& plan_task,
                           std::vector<scan::FileScanTask>* out) {
  std::lock_guard<std::mutex> lock(mu_);
  auto token = tokens_.find(plan_task);
  if (token == tokens_.end()) return false;

  auto plan = plans_.find(token->second.first);
  if (plan == plans_.end()) return false;

  auto& entry = *plan->second;
  if (entry.status != PlanStatus::kCompleted) return false;
  if (token->second.second >= entry.batches.size()) return false;

  entry.last_access = Clock::now();
  *out = entry.batches[token->second.second];
  return true;
}

size_t PlanStore::ExpireIdle(Clock::time_point now) {
  ReapFinished();

  std::lock_guard<std::mutex> lock(mu_);
  size_t expired = 0;
  for (auto it = plans_.begin(); it != plans_.end();) {
    const auto& entry = *it->second;
    if (entry.finished && now - entry.last_access > config_.ttl) {
      DropTokensForLocked(it->first);
      it = plans_.erase(it);
      ++expired;
    } else {
      ++it;
    }
  }
  return expired;
}

size_t PlanStore::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return plans_.size();
}

}  // namespace primeparts::catalog
