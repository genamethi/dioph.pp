#include "primeparts/catalog/rest_scan_plan.h"

#include <thread>
#include <unordered_map>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"

#include "iceberg/catalog/rest/json_serde_internal.h"
#include "iceberg/catalog/rest/types.h"

#include "primeparts/catalog/pp_iceberg_rest.h"

namespace primeparts::catalog {

namespace {

namespace ir = iceberg::rest;
using json = nlohmann::json;

struct TableContext {
  std::unordered_map<int32_t, std::shared_ptr<iceberg::PartitionSpec>> specs;
  std::shared_ptr<iceberg::Schema> schema;
};

bool ContextFor(const iceberg::TableMetadata& metadata, TableContext* out,
                std::string* error) {
  auto schema = metadata.Schema();
  if (!schema.has_value()) {
    if (error) *error = "table schema: " + schema.error().message;
    return false;
  }
  out->schema = schema.value();
  for (const auto& spec : metadata.partition_specs) {
    if (spec) out->specs.emplace(spec->spec_id(), spec);
  }
  return true;
}

std::string PlanPath(const iceberg::Namespace& ns, const std::string& table) {
  return "/v1/namespaces/" + NamespaceUrlPath(ns) + "/tables/" + table +
         "/plan";
}

std::string TasksPath(const iceberg::Namespace& ns, const std::string& table) {
  return "/v1/namespaces/" + NamespaceUrlPath(ns) + "/tables/" + table +
         "/tasks";
}

httplib::Client MakeClient(const std::string& rest_uri) {
  httplib::Client cli(rest_uri);
  cli.set_connection_timeout(2, 0);
  cli.set_read_timeout(30, 0);
  return cli;
}

bool CheckResponse(const httplib::Result& res, const std::string& rest_uri,
                   int expected, std::string* error) {
  if (!res) {
    if (error) *error = "no response from " + rest_uri;
    return false;
  }
  if (res->status == expected) return true;

  std::string message = res->body;
  auto body = json::parse(res->body, nullptr, false);
  if (!body.is_discarded()) {
    if (auto it = body.find("error"); it != body.end()) {
      const auto type = it->value("type", std::string());
      const auto msg = it->value("message", std::string());
      message = type.empty() ? msg : type + ": " + msg;
    }
  }
  if (error) {
    *error = "HTTP " + std::to_string(res->status) + " " + message;
  }
  return false;
}

ir::PlanTableScanRequest ToWireRequest(const scan::ScanPlanRequest& in) {
  ir::PlanTableScanRequest out;
  out.snapshot_id = in.snapshot_id;
  out.select = in.select;
  out.filter = in.filter;
  out.min_rows_requested = in.min_rows_requested;
  out.case_sensitive = in.case_sensitive;
  out.use_snapshot_schema = in.use_snapshot_schema;
  out.start_snapshot_id = in.start_snapshot_id;
  out.end_snapshot_id = in.end_snapshot_id;
  out.stats_fields = in.stats_fields;
  return out;
}

PlanStatus FromWireStatus(ir::PlanStatus status) {
  switch (status) {
    case ir::PlanStatus::kSubmitted:
      return PlanStatus::kSubmitted;
    case ir::PlanStatus::kCompleted:
      return PlanStatus::kCompleted;
    case ir::PlanStatus::kCancelled:
      return PlanStatus::kCancelled;
    case ir::PlanStatus::kFailed:
      return PlanStatus::kFailed;
  }
  return PlanStatus::kFailed;
}

bool ParseJsonBody(const std::string& body, json* out, std::string* error) {
  *out = json::parse(body, nullptr, false);
  if (out->is_discarded()) {
    if (error) *error = "response is not valid JSON";
    return false;
  }
  return true;
}

}  // namespace

bool SubmitTableScan(const std::string& rest_uri, const iceberg::Namespace& ns,
                     const std::string& table,
                     const scan::ScanPlanRequest& request,
                     const iceberg::TableMetadata& metadata,
                     PlanSubmission* out, std::string* error) {
  TableContext ctx;
  if (!ContextFor(metadata, &ctx, error)) return false;

  auto wire = ir::ToJson(ToWireRequest(request));
  if (!wire.has_value()) {
    if (error) *error = "serialize plan request: " + wire.error().message;
    return false;
  }

  auto cli = MakeClient(rest_uri);
  auto res = cli.Post(PlanPath(ns, table), wire.value().dump(),
                      "application/json");
  if (!CheckResponse(res, rest_uri, 200, error)) return false;

  json body;
  if (!ParseJsonBody(res->body, &body, error)) return false;
  auto parsed = ir::PlanTableScanResponseFromJson(body, ctx.specs, *ctx.schema);
  if (!parsed.has_value()) {
    if (error) *error = "parse plan response: " + parsed.error().message;
    return false;
  }

  out->plan_id = parsed.value().plan_id;
  out->status = FromWireStatus(parsed.value().plan_status);
  return true;
}

bool FetchPlanningResult(const std::string& rest_uri,
                         const iceberg::Namespace& ns, const std::string& table,
                         const std::string& plan_id,
                         const iceberg::TableMetadata& metadata,
                         PlanResult* out, std::string* error) {
  TableContext ctx;
  if (!ContextFor(metadata, &ctx, error)) return false;

  auto cli = MakeClient(rest_uri);
  auto res = cli.Get(PlanPath(ns, table) + "/" + plan_id);
  if (!CheckResponse(res, rest_uri, 200, error)) return false;

  json body;
  if (!ParseJsonBody(res->body, &body, error)) return false;
  auto parsed =
      ir::FetchPlanningResultResponseFromJson(body, ctx.specs, *ctx.schema);
  if (!parsed.has_value()) {
    if (error) *error = "parse planning result: " + parsed.error().message;
    return false;
  }

  const auto& value = parsed.value();
  out->status = FromWireStatus(value.plan_status);
  out->tasks.clear();
  out->plan_tasks.clear();
  if (value.file_scan_tasks.has_value()) out->tasks = *value.file_scan_tasks;
  if (value.plan_tasks.has_value()) out->plan_tasks = *value.plan_tasks;
  out->failure = value.error.has_value() ? value.error->message : std::string();
  return true;
}

bool CancelPlanning(const std::string& rest_uri, const iceberg::Namespace& ns,
                    const std::string& table, const std::string& plan_id,
                    std::string* error) {
  auto cli = MakeClient(rest_uri);
  auto res = cli.Delete(PlanPath(ns, table) + "/" + plan_id);
  return CheckResponse(res, rest_uri, 204, error);
}

bool FetchScanTasks(const std::string& rest_uri, const iceberg::Namespace& ns,
                    const std::string& table, const std::string& plan_task,
                    const iceberg::TableMetadata& metadata,
                    std::vector<std::shared_ptr<iceberg::FileScanTask>>* out,
                    std::string* error) {
  TableContext ctx;
  if (!ContextFor(metadata, &ctx, error)) return false;

  ir::FetchScanTasksRequest request;
  request.planTask = plan_task;

  auto cli = MakeClient(rest_uri);
  auto res = cli.Post(TasksPath(ns, table), ir::ToJson(request).dump(),
                      "application/json");
  if (!CheckResponse(res, rest_uri, 200, error)) return false;

  json body;
  if (!ParseJsonBody(res->body, &body, error)) return false;
  auto parsed =
      ir::FetchScanTasksResponseFromJson(body, ctx.specs, *ctx.schema);
  if (!parsed.has_value()) {
    if (error) *error = "parse scan tasks: " + parsed.error().message;
    return false;
  }

  out->clear();
  if (parsed.value().file_scan_tasks.has_value()) {
    *out = *parsed.value().file_scan_tasks;
  }
  return true;
}

bool PlanScanOnServer(const std::string& rest_uri, const iceberg::Namespace& ns,
                      const std::string& table,
                      const scan::ScanPlanRequest& request,
                      const iceberg::TableMetadata& metadata,
                      const PlanPollOptions& poll,
                      std::vector<std::shared_ptr<iceberg::FileScanTask>>* out,
                      std::string* error) {
  out->clear();

  PlanSubmission submission;
  if (!SubmitTableScan(rest_uri, ns, table, request, metadata, &submission,
                       error)) {
    return false;
  }

  PlanResult result;
  result.status = submission.status;
  if (submission.status == PlanStatus::kSubmitted) {
    if (submission.plan_id.empty()) {
      if (error) *error = "server reported 'submitted' without a plan-id";
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + poll.timeout;
    for (;;) {
      if (!FetchPlanningResult(rest_uri, ns, table, submission.plan_id,
                               metadata, &result, error)) {
        return false;
      }
      if (result.status != PlanStatus::kSubmitted) break;
      if (std::chrono::steady_clock::now() >= deadline) {
        if (error) {
          *error = "scan planning did not finish within the poll timeout; "
                   "plan-id " + submission.plan_id;
        }
        return false;
      }
      std::this_thread::sleep_for(poll.interval);
    }
  }

  if (result.status == PlanStatus::kFailed) {
    if (error) {
      *error = "server-side scan planning failed: " +
               (result.failure.empty() ? std::string("no reason given")
                                       : result.failure);
    }
    return false;
  }
  if (result.status == PlanStatus::kCancelled) {
    if (error) *error = "scan planning was cancelled";
    return false;
  }

  *out = std::move(result.tasks);
  for (const auto& token : result.plan_tasks) {
    std::vector<std::shared_ptr<iceberg::FileScanTask>> batch;
    if (!FetchScanTasks(rest_uri, ns, table, token, metadata, &batch, error)) {
      return false;
    }
    out->insert(out->end(), std::make_move_iterator(batch.begin()),
                std::make_move_iterator(batch.end()));
  }
  return true;
}

}  // namespace primeparts::catalog
