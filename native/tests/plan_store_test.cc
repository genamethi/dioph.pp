#include "primeparts/catalog/plan_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

namespace {

using primeparts::catalog::PlanStatus;
using primeparts::catalog::PlanStore;

std::shared_ptr<iceberg::Schema> StoreSchema() {
  std::vector<iceberg::SchemaField> fields;
  fields.push_back(iceberg::SchemaField::MakeRequired(1, "p", iceberg::int64()));
  return std::make_shared<iceberg::Schema>(std::move(fields), 0);
}

primeparts::scan::FileScanTask TaskAt(const std::string& path) {
  auto df = std::make_shared<iceberg::DataFile>();
  df->file_path = path;
  primeparts::scan::FileScanTask task;
  task.inner = std::make_shared<iceberg::FileScanTask>(std::move(df));
  return task;
}

PlanStore::PlanFn PlanOf(size_t task_count) {
  return [task_count](primeparts::scan::ScanPlan* plan, std::string*) {
    plan->table_schema = StoreSchema();
    for (size_t i = 0; i < task_count; ++i) {
      plan->tasks.push_back(TaskAt("f" + std::to_string(i) + ".parquet"));
    }
    return true;
  };
}

PlanStore::Snapshot Settle(PlanStore* store, const std::string& plan_id) {
  PlanStore::Snapshot snap;
  for (int i = 0; i < 2000; ++i) {
    EXPECT_TRUE(store->Fetch(plan_id, &snap));
    if (snap.status != PlanStatus::kSubmitted) return snap;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ADD_FAILURE() << "plan " << plan_id << " never left submitted";
  return snap;
}

TEST(PlanStoreTest, UnknownPlanIdIsDistinguishableFromEveryStatus) {
  PlanStore store;
  PlanStore::Snapshot snap;

  EXPECT_FALSE(store.Fetch("nope", &snap));
  EXPECT_FALSE(store.Cancel("nope"));

  std::vector<primeparts::scan::FileScanTask> tasks;
  EXPECT_FALSE(store.FetchTasks("nope", &tasks));
}

TEST(PlanStoreTest, SubmitReturnsBeforeThePlanCompletes) {
  PlanStore store;
  std::atomic<bool> release{false};

  auto plan_id = store.Submit([&release](primeparts::scan::ScanPlan* plan,
                                         std::string*) {
    while (!release.load()) std::this_thread::yield();
    plan->table_schema = StoreSchema();
    return true;
  });

  PlanStore::Snapshot snap;
  ASSERT_TRUE(store.Fetch(plan_id, &snap));
  EXPECT_EQ(snap.status, PlanStatus::kSubmitted)
      << "Submit must not block on the worker";

  release.store(true);
  EXPECT_EQ(Settle(&store, plan_id).status, PlanStatus::kCompleted);
}

TEST(PlanStoreTest, CompletedPlanUnderTheBatchSizeHasNoPlanTasks) {
  PlanStore store(PlanStore::Config{.batch_tasks = 8});

  auto snap = Settle(&store, store.Submit(PlanOf(5)));

  EXPECT_EQ(snap.status, PlanStatus::kCompleted);
  EXPECT_EQ(snap.tasks.size(), 5u);
  EXPECT_TRUE(snap.plan_tasks.empty())
      << "everything fit inline, so fetchScanTasks has nothing to serve";
  EXPECT_NE(snap.table_schema, nullptr);
}

TEST(PlanStoreTest, TasksBeyondTheBatchSizeBecomePlanTasks) {
  PlanStore store(PlanStore::Config{.batch_tasks = 4});

  const auto plan_id = store.Submit(PlanOf(10));
  auto snap = Settle(&store, plan_id);

  ASSERT_EQ(snap.status, PlanStatus::kCompleted);
  EXPECT_EQ(snap.tasks.size(), 4u);
  ASSERT_EQ(snap.plan_tasks.size(), 2u) << "10 tasks in batches of 4";

  std::vector<primeparts::scan::FileScanTask> second;
  ASSERT_TRUE(store.FetchTasks(snap.plan_tasks[0], &second));
  EXPECT_EQ(second.size(), 4u);

  std::vector<primeparts::scan::FileScanTask> third;
  ASSERT_TRUE(store.FetchTasks(snap.plan_tasks[1], &third));
  EXPECT_EQ(third.size(), 2u) << "the trailing batch is short";
}

TEST(PlanStoreTest, FetchingIsNonDestructive) {
  PlanStore store(PlanStore::Config{.batch_tasks = 2});

  const auto plan_id = store.Submit(PlanOf(4));
  auto snap = Settle(&store, plan_id);
  ASSERT_EQ(snap.plan_tasks.size(), 1u);

  std::vector<primeparts::scan::FileScanTask> first;
  std::vector<primeparts::scan::FileScanTask> again;
  ASSERT_TRUE(store.FetchTasks(snap.plan_tasks[0], &first));
  ASSERT_TRUE(store.FetchTasks(snap.plan_tasks[0], &again));
  EXPECT_EQ(first.size(), again.size())
      << "a re-fetch serves the same batch until cancel or expiry";
}

TEST(PlanStoreTest, FailedPlanCarriesItsError) {
  PlanStore store;

  auto plan_id = store.Submit([](primeparts::scan::ScanPlan*,
                                 std::string* error) {
    *error = "manifest unreadable";
    return false;
  });
  auto snap = Settle(&store, plan_id);

  EXPECT_EQ(snap.status, PlanStatus::kFailed);
  EXPECT_EQ(snap.error, "manifest unreadable");
  EXPECT_TRUE(snap.tasks.empty());
}

TEST(PlanStoreTest, CancelKeepsThePlanIdAnswerable) {
  PlanStore store(PlanStore::Config{.batch_tasks = 2});

  const auto plan_id = store.Submit(PlanOf(4));
  auto snap = Settle(&store, plan_id);
  ASSERT_EQ(snap.status, PlanStatus::kCompleted);
  const auto token = snap.plan_tasks.at(0);

  ASSERT_TRUE(store.Cancel(plan_id));

  PlanStore::Snapshot after;
  ASSERT_TRUE(store.Fetch(plan_id, &after))
      << "the spec has a cancelled status, so the plan-id must still answer";
  EXPECT_EQ(after.status, PlanStatus::kCancelled);
  EXPECT_TRUE(after.tasks.empty());

  std::vector<primeparts::scan::FileScanTask> tasks;
  EXPECT_FALSE(store.FetchTasks(token, &tasks))
      << "plan-task tokens die with their plan";
}

TEST(PlanStoreTest, CancellingARunningPlanDiscardsItsResult) {
  PlanStore store;
  std::atomic<bool> release{false};
  std::atomic<bool> produced{false};

  auto plan_id = store.Submit([&release, &produced](
                                  primeparts::scan::ScanPlan* plan,
                                  std::string*) {
    while (!release.load()) std::this_thread::yield();
    plan->table_schema = StoreSchema();
    plan->tasks.push_back(TaskAt("late.parquet"));
    produced.store(true);
    return true;
  });

  ASSERT_TRUE(store.Cancel(plan_id));
  release.store(true);
  while (!produced.load()) std::this_thread::yield();

  PlanStore::Snapshot snap;
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(store.Fetch(plan_id, &snap));
    if (snap.status == PlanStatus::kCancelled && snap.tasks.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(snap.status, PlanStatus::kCancelled);
  EXPECT_TRUE(snap.tasks.empty())
      << "work that lands after cancellation is thrown away, not published";
}

TEST(PlanStoreTest, IdlePlansExpireAndTakeTheirTokensWithThem) {
  PlanStore store(
      PlanStore::Config{.batch_tasks = 2, .ttl = std::chrono::seconds(300)});

  const auto plan_id = store.Submit(PlanOf(4));
  auto snap = Settle(&store, plan_id);
  ASSERT_EQ(snap.plan_tasks.size(), 1u);
  const auto token = snap.plan_tasks.at(0);

  EXPECT_EQ(store.ExpireIdle(PlanStore::Clock::now()), 0u)
      << "a freshly touched plan is not idle";
  EXPECT_EQ(store.size(), 1u);

  EXPECT_EQ(
      store.ExpireIdle(PlanStore::Clock::now() + std::chrono::seconds(301)),
      1u);
  EXPECT_EQ(store.size(), 0u);

  PlanStore::Snapshot gone;
  EXPECT_FALSE(store.Fetch(plan_id, &gone));

  std::vector<primeparts::scan::FileScanTask> tasks;
  EXPECT_FALSE(store.FetchTasks(token, &tasks));
}

TEST(PlanStoreTest, ARunningPlanIsNeverExpired) {
  PlanStore store(PlanStore::Config{.ttl = std::chrono::seconds(0)});
  std::atomic<bool> release{false};

  auto plan_id = store.Submit([&release](primeparts::scan::ScanPlan*,
                                         std::string*) {
    while (!release.load()) std::this_thread::yield();
    return true;
  });

  EXPECT_EQ(
      store.ExpireIdle(PlanStore::Clock::now() + std::chrono::seconds(3600)),
      0u)
      << "evicting a plan whose worker is still running would orphan it";
  EXPECT_EQ(store.size(), 1u);

  release.store(true);
  Settle(&store, plan_id);
}

TEST(PlanStoreTest, ConcurrentSubmitsGetDistinctIdsAndResults) {
  PlanStore store(PlanStore::Config{.batch_tasks = 100});
  constexpr int kPlans = 16;

  std::vector<std::string> ids(kPlans);
  std::vector<std::thread> submitters;
  for (int i = 0; i < kPlans; ++i) {
    submitters.emplace_back([&store, &ids, i]() {
      ids[i] = store.Submit(PlanOf(static_cast<size_t>(i) + 1));
    });
  }
  for (auto& thread : submitters) thread.join();

  std::set<std::string> distinct(ids.begin(), ids.end());
  EXPECT_EQ(distinct.size(), static_cast<size_t>(kPlans))
      << "plan-ids are random, not a shared sequence";

  for (int i = 0; i < kPlans; ++i) {
    auto snap = Settle(&store, ids[i]);
    EXPECT_EQ(snap.status, PlanStatus::kCompleted);
    EXPECT_EQ(snap.tasks.size(), static_cast<size_t>(i) + 1)
        << "plan " << i << " got another plan's result";
  }
}

}  // namespace
