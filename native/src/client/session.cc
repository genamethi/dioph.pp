#include "primeparts/client/session.h"

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/compute/initialize.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/expression/expression.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/expression/predicate.h"
#include "iceberg/expression/term.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/common/arrow_init.h"
#include "primeparts/scan/scan_planner.h"
#include "primeparts/source_scan.h"

namespace primeparts::client {

namespace {

constexpr const char* kDefaultRestUri = "http://127.0.0.1:8181";

std::string ResolveRestUri(const std::string& given) {
  if (!given.empty()) return given;
  const char* env = std::getenv("PRIMEPARTS_REST_URI");
  return env ? env : kDefaultRestUri;
}

const iceberg::SchemaField* FieldByName(const iceberg::Schema& schema,
                                        const std::string& name) {
  for (const auto& field : schema.fields()) {
    if (field.name() == name) return &field;
  }
  return nullptr;
}

const iceberg::SchemaField* FieldById(const iceberg::Schema& schema,
                                      int32_t field_id) {
  for (const auto& field : schema.fields()) {
    if (field.field_id() == field_id) return &field;
  }
  return nullptr;
}

bool ProjectSchema(const std::shared_ptr<iceberg::Schema>& table_schema,
                   const std::vector<std::string>& select,
                   std::shared_ptr<iceberg::Schema>* out, std::string* error) {
  if (select.empty()) {
    *out = table_schema;
    return true;
  }
  std::vector<iceberg::SchemaField> fields;
  fields.reserve(select.size());
  for (const auto& name : select) {
    const auto* field = FieldByName(*table_schema, name);
    if (field == nullptr) {
      if (error) *error = "select column not in table schema: " + name;
      return false;
    }
    fields.push_back(*field);
  }
  *out = std::make_shared<iceberg::Schema>(std::move(fields),
                                           table_schema->schema_id());
  return true;
}

bool LiteralToScalar(const iceberg::Literal& literal,
                     std::shared_ptr<arrow::Scalar>* out, std::string* error) {
  switch (literal.type()->type_id()) {
    case iceberg::TypeId::kInt:
      *out = std::make_shared<arrow::Int32Scalar>(
          std::get<int32_t>(literal.value()));
      return true;
    case iceberg::TypeId::kLong:
      *out = std::make_shared<arrow::Int64Scalar>(
          std::get<int64_t>(literal.value()));
      return true;
    default:
      if (error)
        *error = "residual evaluation supports int and long literals only";
      return false;
  }
}

const char* CompareFunction(iceberg::Expression::Operation op) {
  switch (op) {
    case iceberg::Expression::Operation::kLt: return "less";
    case iceberg::Expression::Operation::kLtEq: return "less_equal";
    case iceberg::Expression::Operation::kGt: return "greater";
    case iceberg::Expression::Operation::kGtEq: return "greater_equal";
    case iceberg::Expression::Operation::kEq: return "equal";
    case iceberg::Expression::Operation::kNotEq: return "not_equal";
    default: return nullptr;
  }
}

bool EvaluateMask(const iceberg::Expression& expr,
                  const arrow::RecordBatch& batch, arrow::Datum* out,
                  std::string* error) {
  using Operation = iceberg::Expression::Operation;
  const Operation op = expr.op();
  if (op == Operation::kTrue) {
    *out = arrow::Datum(std::make_shared<arrow::BooleanScalar>(true));
    return true;
  }
  if (op == Operation::kFalse) {
    *out = arrow::Datum(std::make_shared<arrow::BooleanScalar>(false));
    return true;
  }
  if (op == Operation::kAnd || op == Operation::kOr) {
    const auto* logical = dynamic_cast<const iceberg::And*>(&expr);
    const iceberg::Expression* left = nullptr;
    const iceberg::Expression* right = nullptr;
    if (logical != nullptr) {
      left = logical->left().get();
      right = logical->right().get();
    } else {
      const auto* orExpr = dynamic_cast<const iceberg::Or*>(&expr);
      if (orExpr == nullptr) {
        if (error) *error = "unrecognized logical expression shape";
        return false;
      }
      left = orExpr->left().get();
      right = orExpr->right().get();
    }
    arrow::Datum lhs;
    arrow::Datum rhs;
    if (!EvaluateMask(*left, batch, &lhs, error)) return false;
    if (!EvaluateMask(*right, batch, &rhs, error)) return false;
    std::vector<arrow::Datum> operands{lhs, rhs};
    auto combined = arrow::compute::CallFunction(
        op == Operation::kAnd ? "and_kleene" : "or_kleene", operands);
    if (!combined.ok()) {
      if (error) *error = combined.status().ToString();
      return false;
    }
    *out = *combined;
    return true;
  }
  if (op == Operation::kNot) {
    const auto* notExpr = dynamic_cast<const iceberg::Not*>(&expr);
    if (notExpr == nullptr) {
      if (error) *error = "unrecognized not expression shape";
      return false;
    }
    arrow::Datum inner;
    if (!EvaluateMask(*notExpr->child(), batch, &inner, error)) return false;
    std::vector<arrow::Datum> operand{inner};
    auto inverted = arrow::compute::CallFunction("invert", operand);
    if (!inverted.ok()) {
      if (error) *error = inverted.status().ToString();
      return false;
    }
    *out = *inverted;
    return true;
  }
  const char* function = CompareFunction(op);
  const auto* predicate = dynamic_cast<const iceberg::UnboundPredicate*>(&expr);
  if (function == nullptr || predicate == nullptr) {
    if (error)
      *error = "residual evaluation does not support expression op " +
               std::string(iceberg::ToString(op));
    return false;
  }
  const auto reference = predicate->reference();
  if (reference == nullptr) {
    if (error) *error = "predicate without a named reference";
    return false;
  }
  const std::string column_name(reference->name());
  auto column = batch.GetColumnByName(column_name);
  if (column == nullptr) {
    if (error)
      *error = "residual references a column outside the projection: " +
               column_name;
    return false;
  }
  const auto literals = predicate->literals();
  if (literals.size() != 1) {
    if (error) *error = "residual evaluation supports unary predicates only";
    return false;
  }
  std::shared_ptr<arrow::Scalar> scalar;
  if (!LiteralToScalar(literals[0], &scalar, error)) return false;
  std::vector<arrow::Datum> args{arrow::Datum(column), arrow::Datum(scalar)};
  auto mask = arrow::compute::CallFunction(function, args);
  if (!mask.ok()) {
    if (error) *error = mask.status().ToString();
    return false;
  }
  *out = *mask;
  return true;
}

bool ApplyResidual(const std::shared_ptr<iceberg::Expression>& residual,
                   std::shared_ptr<arrow::RecordBatch>* batch,
                   std::string* error) {
  if (!residual || residual->op() == iceberg::Expression::Operation::kTrue) {
    return true;
  }
  if (residual->op() == iceberg::Expression::Operation::kFalse) {
    *batch = (*batch)->Slice(0, 0);
    return true;
  }
  arrow::Datum mask;
  if (!EvaluateMask(*residual, **batch, &mask, error)) return false;
  if (mask.is_scalar()) {
    const auto& scalar =
        static_cast<const arrow::BooleanScalar&>(*mask.scalar());
    if (!scalar.is_valid || !scalar.value) *batch = (*batch)->Slice(0, 0);
    return true;
  }
  auto filtered = arrow::compute::Filter(arrow::Datum(*batch), mask);
  if (!filtered.ok()) {
    if (error) *error = filtered.status().ToString();
    return false;
  }
  *batch = filtered->record_batch();
  return true;
}

bool BuildServerPlan(const std::string& rest_uri, const iceberg::Namespace& ns,
                     const std::string& table,
                     const std::shared_ptr<iceberg::TableMetadata>& metadata,
                     const scan::ScanPlanRequest& request,
                     const catalog::PlanPollOptions& poll, scan::ScanPlan* out,
                     std::string* error) {
  *out = scan::ScanPlan{};
  if (!scan::TableReadTraits::FromMetadata(*metadata, &out->traits, error)) {
    return false;
  }
  auto schema_r = metadata->Schema();
  if (!schema_r.has_value()) {
    if (error) *error = "TableMetadata::Schema: " + schema_r.error().message;
    return false;
  }
  out->table_schema = schema_r.value();
  if (!ProjectSchema(out->table_schema, request.select, &out->projected_schema,
                     error)) {
    return false;
  }
  out->residual = request.filter;
  if (out->traits.sorted() && out->traits.sort_keys.front().ascending) {
    const auto& key = out->traits.sort_keys.front();
    const auto* field = FieldById(*out->table_schema, key.field_id);
    if (field != nullptr) {
      auto key_type =
          std::dynamic_pointer_cast<iceberg::PrimitiveType>(field->type());
      scan::DeriveKeyWindow(request.filter, key.name, key_type, &out->key_lo,
                            &out->key_hi);
    }
  }
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  if (!catalog::PlanScanOnServer(rest_uri, ns, table, request, *metadata, poll,
                                 &tasks, error)) {
    return false;
  }
  out->tasks.reserve(tasks.size());
  for (auto& task : tasks) {
    scan::FileScanTask planned;
    planned.planned_rows =
        static_cast<int64_t>(task->data_file()->record_count);
    out->planned_rows += planned.planned_rows;
    planned.inner = std::move(task);
    out->tasks.push_back(std::move(planned));
  }
  return true;
}

}  // namespace

struct TableHandle::State {
  std::string name;
  std::shared_ptr<iceberg::TableMetadata> metadata;
  catalog::ScanPlanningMode planning_mode = catalog::ScanPlanningMode::kClient;
};

const std::string& TableHandle::name() const { return state_->name; }

const std::shared_ptr<iceberg::TableMetadata>& TableHandle::metadata() const {
  return state_->metadata;
}

catalog::ScanPlanningMode TableHandle::planning_mode() const {
  return state_->planning_mode;
}

struct ScanStream::Impl {
  scan::ScanPlan plan;
  std::shared_ptr<iceberg::FileIO> io;
  catalog::ScanPlanningMode planned_via = catalog::ScanPlanningMode::kClient;
  int shards = 1;

  std::vector<std::thread> workers;
  std::mutex mu;
  std::condition_variable produced;
  std::condition_variable consumed;
  std::deque<std::shared_ptr<arrow::RecordBatch>> queue;
  std::string first_error;
  int active_workers = 0;
  bool stopping = false;

  std::unique_ptr<primeparts::SourceTableReader> single;

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mu);
      stopping = true;
    }
    consumed.notify_all();
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }
  }

  void RunShard(int shard_index) {
    std::string error;
    auto reader = primeparts::SourceTableReader::Open(plan, io, &error,
                                                      shard_index, shards);
    if (!reader) {
      Fail(error.empty() ? "SourceTableReader::Open failed" : error);
      return;
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (!reader->Next(&batch, &error)) {
        if (!error.empty()) Fail(error);
        break;
      }
      if (!batch) break;
      if (!ApplyResidual(plan.residual, &batch, &error)) {
        Fail(error);
        break;
      }
      if (batch->num_rows() == 0) continue;
      std::unique_lock<std::mutex> lock(mu);
      consumed.wait(lock, [this] {
        return stopping || queue.size() < static_cast<size_t>(4 * shards);
      });
      if (stopping) break;
      queue.push_back(std::move(batch));
      produced.notify_one();
    }
    std::lock_guard<std::mutex> lock(mu);
    --active_workers;
    produced.notify_all();
  }

  void Fail(const std::string& error) {
    std::lock_guard<std::mutex> lock(mu);
    if (first_error.empty()) first_error = error;
  }
};

ScanStream::ScanStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ScanStream::~ScanStream() = default;

bool ScanStream::Next(std::shared_ptr<arrow::RecordBatch>* out,
                      std::string* error) {
  out->reset();
  if (impl_->single) {
    std::shared_ptr<arrow::RecordBatch> batch;
    while (impl_->single->Next(&batch, error)) {
      if (!batch) return false;
      if (!ApplyResidual(impl_->plan.residual, &batch, error)) return false;
      if (batch->num_rows() == 0) continue;
      *out = std::move(batch);
      return true;
    }
    return false;
  }
  std::unique_lock<std::mutex> lock(impl_->mu);
  impl_->produced.wait(lock, [this] {
    return !impl_->queue.empty() || impl_->active_workers == 0 ||
           !impl_->first_error.empty();
  });
  if (!impl_->queue.empty()) {
    *out = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    impl_->consumed.notify_one();
    return true;
  }
  if (!impl_->first_error.empty()) {
    if (error) *error = impl_->first_error;
    return false;
  }
  return false;
}

int64_t ScanStream::planned_rows() const { return impl_->plan.planned_rows; }

int64_t ScanStream::file_count() const {
  return static_cast<int64_t>(impl_->plan.tasks.size());
}

int ScanStream::shard_count() const { return impl_->shards; }

catalog::ScanPlanningMode ScanStream::planned_via() const {
  return impl_->planned_via;
}

struct Session::Impl {
  SessionOptions options;
  iceberg::Namespace ns;
  std::shared_ptr<iceberg::Catalog> catalog;
  std::shared_ptr<iceberg::FileIO> io;
};

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Session::~Session() = default;

std::unique_ptr<Session> Session::Open(const SessionOptions& options,
                                       std::string* error) {
  primeparts::common::EnsureArrowRegistration();
  {
    static const arrow::Status compute_status = arrow::compute::Initialize();
    if (!compute_status.ok()) {
      if (error)
        *error = "arrow::compute::Initialize: " + compute_status.ToString();
      return nullptr;
    }
  }
  if (options.warehouse.empty()) {
    if (error) *error = "SessionOptions.warehouse is required";
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->options.rest_uri = ResolveRestUri(options.rest_uri);
  if (impl->options.scan_threads < 1) impl->options.scan_threads = 1;
  impl->ns = catalog::ResolveNamespace(options.ns);
  std::string mode;
  impl->catalog = catalog::OpenCatalog(options.warehouse,
                                       impl->options.rest_uri, &mode, error);
  if (!impl->catalog) return nullptr;
  impl->io = catalog::LocalIO();
  return std::unique_ptr<Session>(new Session(std::move(impl)));
}

bool Session::LoadTable(const std::string& table, TableHandle* out,
                        std::string* error) {
  iceberg::TableIdentifier ident{.ns = impl_->ns, .name = table};
  auto loaded = impl_->catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    if (error) *error = "LoadTable " + table + ": " + loaded.error().message;
    return false;
  }
  auto state = std::make_shared<TableHandle::State>();
  state->name = table;
  state->metadata = loaded.value()->metadata();
  if (!state->metadata) {
    if (error) *error = "LoadTable " + table + ": no metadata";
    return false;
  }
  if (!catalog::FetchScanPlanningMode(impl_->options.rest_uri, impl_->ns,
                                      table, &state->planning_mode, error)) {
    return false;
  }
  out->state_ = std::move(state);
  return true;
}

std::unique_ptr<ScanStream> Session::Scan(const TableHandle& table,
                                          const scan::ScanPlanRequest& request,
                                          std::string* error) {
  auto impl = std::make_unique<ScanStream::Impl>();
  impl->io = impl_->io;
  impl->planned_via = table.planning_mode();
  if (table.planning_mode() == catalog::ScanPlanningMode::kServer) {
    if (!BuildServerPlan(impl_->options.rest_uri, impl_->ns, table.name(),
                         table.metadata(), request, impl_->options.poll,
                         &impl->plan, error)) {
      return nullptr;
    }
  } else {
    if (!scan::PlanTableScan(table.metadata(), impl_->io, request, &impl->plan,
                             error)) {
      return nullptr;
    }
  }
  impl->plan.read_batch_size = impl_->options.read_batch_size;
  const int files = static_cast<int>(impl->plan.tasks.size());
  impl->shards = impl_->options.scan_threads;
  if (files > 0 && impl->shards > files) impl->shards = files;
  if (impl->shards < 1) impl->shards = 1;
  if (impl->shards == 1) {
    impl->single = primeparts::SourceTableReader::Open(impl->plan, impl_->io,
                                                       error, 0, 1);
    if (!impl->single) return nullptr;
  } else {
    impl->active_workers = impl->shards;
    for (int shard = 0; shard < impl->shards; ++shard) {
      impl->workers.emplace_back(
          [raw = impl.get(), shard] { raw->RunShard(shard); });
    }
  }
  return std::unique_ptr<ScanStream>(new ScanStream(std::move(impl)));
}

bool Session::FieldUpperBound(const TableHandle& table,
                              const std::string& field, int64_t* out,
                              bool* present, std::string* error) {
  return catalog::FetchFieldUpperBound(impl_->options.rest_uri, impl_->ns,
                                       table.name(), field, out, present,
                                       error);
}

const iceberg::Namespace& Session::ns() const { return impl_->ns; }

const std::string& Session::rest_uri() const {
  return impl_->options.rest_uri;
}

const std::shared_ptr<iceberg::Catalog>& Session::catalog() const {
  return impl_->catalog;
}

const std::shared_ptr<iceberg::FileIO>& Session::io() const {
  return impl_->io;
}

}  // namespace primeparts::client
