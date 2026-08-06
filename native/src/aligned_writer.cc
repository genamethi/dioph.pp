#include "primeparts/aligned_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <arrow/api.h>

#include "iceberg/catalog.h"
#include "iceberg/expression/literal.h"
#include "iceberg/result.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/snapshot.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"

#include "primeparts/catalog/partition_stats.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/writer.h"

namespace primeparts {

namespace {

constexpr double kEwmaAlpha = 0.3;

const int64_t* Int64Col(const arrow::RecordBatch& batch, const std::string& name) {
  auto col = batch.GetColumnByName(name);
  if (!col || col->type_id() != arrow::Type::INT64) return nullptr;
  return static_cast<const arrow::Int64Array&>(*col).raw_values();
}

int64_t UpperBoundByKey(const arrow::RecordBatch& batch, const std::string& key,
                        int64_t cut_key) {
  const int64_t* p = Int64Col(batch, key);
  const int64_t n = batch.num_rows();
  if (!p) return n;
  return static_cast<int64_t>(std::upper_bound(p, p + n, cut_key) - p);
}

constexpr char kFileTargetKey[] = "write.target-file-size-bytes";
constexpr char kRowGroupTargetKey[] = "write.parquet.row-group-size-bytes";
constexpr char kRgsPerFileKey[] = "pp.write.rgs-per-file";
constexpr char kBucketTargetKey[] = "pp.buckets.target-bytes";
constexpr char kBucketVersionKey[] = "pp.buckets.version";
constexpr char kRefBytesPerRowKey[] = "pp.write.ref-bytes-per-row-prior";

bool ReadProperty(const std::unordered_map<std::string, std::string>& props,
                  const char* key, std::vector<std::string>* absent,
                  std::string* error, const std::function<bool(const std::string&)>& set) {
  auto it = props.find(key);
  if (it == props.end()) {
    absent->emplace_back(key);
    return true;
  }
  if (!set(it->second)) {
    if (error) *error = std::string(key) + ": cannot parse '" + it->second + "'";
    return false;
  }
  return true;
}

}  // namespace

std::unordered_map<std::string, std::string> ShapePolicy::AsTableProperties()
    const {
  return {
      {kFileTargetKey, std::to_string(file_target_bytes)},
      {kRowGroupTargetKey, std::to_string(rg_target_bytes())},
      {kRgsPerFileKey, std::to_string(rgs_per_file)},
      {kBucketTargetKey, std::to_string(bucket_target_bytes)},
      {kBucketVersionKey, std::to_string(bucket_version)},
      {kRefBytesPerRowKey, std::to_string(ref_bytes_per_row_prior)},
  };
}

bool ShapePolicy::FromTableProperties(
    const std::unordered_map<std::string, std::string>& properties,
    std::vector<std::string>* absent, std::string* error) {
  absent->clear();
  auto as_int64 = [](const std::string& s, int64_t* out) {
    try {
      size_t used = 0;
      const int64_t v = std::stoll(s, &used);
      if (used != s.size()) return false;
      *out = v;
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  auto as_double = [](const std::string& s, double* out) {
    try {
      size_t used = 0;
      const double v = std::stod(s, &used);
      if (used != s.size()) return false;
      *out = v;
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };

  if (!ReadProperty(properties, kFileTargetKey, absent, error,
                    [&](const std::string& v) {
                      return as_int64(v, &file_target_bytes);
                    }))
    return false;
  if (!ReadProperty(properties, kRgsPerFileKey, absent, error,
                    [&](const std::string& v) {
                      int64_t n = 0;
                      if (!as_int64(v, &n) || n <= 0) return false;
                      rgs_per_file = static_cast<int>(n);
                      return true;
                    }))
    return false;
  if (!ReadProperty(properties, kBucketTargetKey, absent, error,
                    [&](const std::string& v) {
                      return as_int64(v, &bucket_target_bytes);
                    }))
    return false;
  if (!ReadProperty(properties, kBucketVersionKey, absent, error,
                    [&](const std::string& v) {
                      int64_t n = 0;
                      if (!as_int64(v, &n)) return false;
                      bucket_version = static_cast<int32_t>(n);
                      return true;
                    }))
    return false;
  if (!ReadProperty(properties, kRefBytesPerRowKey, absent, error,
                    [&](const std::string& v) {
                      return as_double(v, &ref_bytes_per_row_prior);
                    }))
    return false;
  return true;
}

struct AlignedBucketWriter::Impl {
  fs::path warehouse;
  iceberg::Namespace ns;
  std::vector<BoundTable> tables;
  AtomKey atom;
  ShapePolicy policy;
  int ref_idx = -1;

  int32_t bucket = 0;
  int64_t bucket_ref_bytes = 0;
  int rgs_completed_in_file = 0;

  double ref_bpr = 1.1;
  int64_t rg_ref_bytes_est = 0;
  int64_t ref_atoms_in_rg = 0;

  std::vector<std::unique_ptr<BucketParquetWriter>> writers;
  std::vector<std::vector<WrittenFile>> all_files;
  std::vector<int32_t> next_seq;

  bool OpenBucketWriters(std::string* error);
  bool CloseBucketWriters(std::string* error);
  bool WriteSlice(size_t t, const arrow::RecordBatch& batch, int64_t start,
                  int64_t end, std::string* error);
  bool RgFill(std::string* error);
};

bool AlignedBucketWriter::Impl::OpenBucketWriters(std::string* error) {
  writers.clear();
  writers.resize(tables.size());
  const std::string vdir =
      "p_bucket_version=" + std::to_string(policy.bucket_version);
  const std::string bdir = "p_bucket=" + std::to_string(bucket);
  auto partition_values = std::make_shared<iceberg::PartitionValues>(
      std::vector<iceberg::Literal>{iceberg::Literal::Int(policy.bucket_version),
                                    iceberg::Literal::Int(bucket)});
  for (size_t t = 0; t < tables.size(); ++t) {
    const auto& bt = tables[t];
    WriterConfig cfg;
    cfg.output_dir = catalog::StagingDataDir(warehouse, ns, bt.name) / vdir / bdir;
    cfg.schema = bt.schema;
    cfg.table_name = bt.name;
    cfg.filename_prefix = bt.name;
    cfg.delta_columns = bt.delta_columns;
    cfg.stat_columns = bt.stat_columns;
    cfg.partition_spec = bt.spec;
    cfg.partition_values = partition_values;
    cfg.bucket_version = policy.bucket_version;
    cfg.bucket = bucket;
    cfg.target_rows_per_file = 0;
    cfg.max_row_group_rows = INT64_MAX;
    cfg.starting_file_seq = next_seq[t];
    auto w = BucketParquetWriter::Make(std::move(cfg), error);
    if (!w) return false;
    writers[t] = std::move(w);
  }
  return true;
}

bool AlignedBucketWriter::Impl::CloseBucketWriters(std::string* error) {
  for (size_t t = 0; t < tables.size(); ++t) {
    if (!writers[t]) continue;
    std::vector<WrittenFile> files;
    if (!writers[t]->Close(&files, error)) return false;
    all_files[t].insert(all_files[t].end(), files.begin(), files.end());
    writers[t].reset();
  }
  return true;
}

bool AlignedBucketWriter::Impl::WriteSlice(size_t t,
                                           const arrow::RecordBatch& batch,
                                           int64_t start, int64_t end,
                                           std::string* error) {
  if (end <= start) return true;
  auto slice = batch.Slice(start, end - start);
  return writers[t]->Write(*slice, error);
}

bool AlignedBucketWriter::Impl::RgFill(std::string* error) {
  ++rgs_completed_in_file;
  if (rgs_completed_in_file < policy.rgs_per_file) {
    for (size_t t = 0; t < tables.size(); ++t) {
      int64_t flushed = 0;
      if (!writers[t]->CutRowGroup(&flushed, error)) return false;
      if (static_cast<int>(t) == ref_idx && ref_atoms_in_rg > 0) {
        double measured = static_cast<double>(flushed) /
                          static_cast<double>(ref_atoms_in_rg);
        ref_bpr = kEwmaAlpha * measured + (1.0 - kEwmaAlpha) * ref_bpr;
      }
    }
  } else {
    int64_t ref_file_bytes = 0;
    for (size_t t = 0; t < tables.size(); ++t) {
      int64_t fb = 0;
      if (!writers[t]->RollFile(error, &fb)) return false;
      if (static_cast<int>(t) == ref_idx) ref_file_bytes = fb;
    }
    rgs_completed_in_file = 0;
    bucket_ref_bytes += ref_file_bytes;
    if (bucket_ref_bytes >= policy.bucket_target_bytes) {
      if (!CloseBucketWriters(error)) return false;
      ++bucket;
      bucket_ref_bytes = 0;
      next_seq.assign(tables.size(), 0);
      if (!OpenBucketWriters(error)) return false;
    }
  }
  rg_ref_bytes_est = 0;
  ref_atoms_in_rg = 0;
  return true;
}

std::unique_ptr<AlignedBucketWriter> AlignedBucketWriter::Make(
    const fs::path& warehouse, const iceberg::Namespace& ns,
    std::vector<BoundTable> tables, AtomKey atom, ShapePolicy policy,
    ResumeState resume, std::string* error) {
  auto impl = std::make_unique<Impl>();
  impl->warehouse = warehouse;
  impl->ns = ns;
  impl->tables = std::move(tables);
  impl->atom = std::move(atom);
  impl->policy = policy;
  impl->ref_bpr =
      policy.ref_bytes_per_row_prior > 0 ? policy.ref_bytes_per_row_prior : 1.1;
  impl->bucket = resume.bucket;
  impl->bucket_ref_bytes = resume.bucket_bytes;

  int ref_count = 0;
  for (size_t t = 0; t < impl->tables.size(); ++t) {
    if (impl->tables[t].reference) {
      impl->ref_idx = static_cast<int>(t);
      ++ref_count;
    }
  }
  if (ref_count != 1) {
    if (error) *error = "AlignedBucketWriter requires exactly one reference table";
    return nullptr;
  }

  impl->all_files.assign(impl->tables.size(), {});
  impl->next_seq.assign(impl->tables.size(), 0);
  for (size_t t = 0; t < impl->tables.size(); ++t) {
    auto it = resume.next_seq.find(impl->tables[t].name);
    if (it != resume.next_seq.end()) impl->next_seq[t] = it->second;
  }
  if (!impl->OpenBucketWriters(error)) return nullptr;
  return std::unique_ptr<AlignedBucketWriter>(
      new AlignedBucketWriter(std::move(impl)));
}

AlignedBucketWriter::AlignedBucketWriter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
AlignedBucketWriter::~AlignedBucketWriter() = default;

bool AlignedBucketWriter::Append(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
    std::string* error) {
  auto& I = *impl_;
  if (batches.size() != I.tables.size()) {
    if (error) *error = "Append batch count != bound table count";
    return false;
  }
  const auto& ref_batch = *batches[I.ref_idx];
  const int64_t R = ref_batch.num_rows();
  if (R == 0) return true;
  const int64_t* ref_p = Int64Col(ref_batch, I.atom.column);
  if (!ref_p) {
    if (error) *error = "reference batch missing key column " + I.atom.column;
    return false;
  }

  std::vector<int64_t> cursor(I.tables.size(), 0);
  const int64_t rg_target = I.policy.rg_target_bytes();
  int64_t a = 0;
  while (a < R) {
    const int64_t remaining = rg_target - I.rg_ref_bytes_est;
    int64_t budget =
        remaining > 0
            ? static_cast<int64_t>(std::floor(remaining / std::max(I.ref_bpr, 1e-9)))
            : 0;
    if (budget < 1) budget = 1;
    const int64_t a_next = std::min(a + budget, R);
    const int64_t cut_key = ref_p[a_next - 1];

    if (!I.WriteSlice(I.ref_idx, ref_batch, a, a_next, error)) return false;
    for (size_t t = 0; t < I.tables.size(); ++t) {
      if (static_cast<int>(t) == I.ref_idx) continue;
      const auto& tb = *batches[t];
      const int64_t end_t = I.tables[t].align
                                ? I.tables[t].align(tb, I.atom.column, cut_key)
                                : UpperBoundByKey(tb, I.atom.column, cut_key);
      if (!I.WriteSlice(t, tb, cursor[t], end_t, error)) return false;
      cursor[t] = end_t;
    }

    const int64_t atoms = a_next - a;
    I.ref_atoms_in_rg += atoms;
    I.rg_ref_bytes_est +=
        static_cast<int64_t>(std::llround(atoms * I.ref_bpr));
    a = a_next;
    if (I.rg_ref_bytes_est >= rg_target) {
      if (!I.RgFill(error)) return false;
    }
  }
  return true;
}

bool AlignedBucketWriter::Finish(CommitPlan* out, std::string* error) {
  auto& I = *impl_;
  if (!I.CloseBucketWriters(error)) return false;
  out->tables.clear();
  for (size_t t = 0; t < I.tables.size(); ++t) {
    out->tables.push_back(TableFiles{I.tables[t].name, std::move(I.all_files[t])});
  }
  return true;
}

namespace {

struct BucketStats {
  int32_t frontier = 0;
  int64_t frontier_bytes = 0;
  int32_t frontier_files = 0;
  bool any = false;
};

bool BucketStatsForTable(const iceberg::Table& table, const std::string& name,
                         const BucketFields& bucket_fields,
                         int32_t bucket_version, BucketStats* out,
                         std::string* error) {
  catalog::PartitionStatsSet stats;
  if (!catalog::LoadPartitionStats(table, &stats, error)) {
    if (error) *error = name + ": " + *error;
    return false;
  }
  int v_pos = -1;
  int b_pos = -1;
  for (size_t i = 0; i < stats.field_names.size(); ++i) {
    if (stats.field_names[i] == bucket_fields.version_field) {
      v_pos = static_cast<int>(i);
    } else if (stats.field_names[i] == bucket_fields.bucket_field) {
      b_pos = static_cast<int>(i);
    }
  }
  if (v_pos < 0 || b_pos < 0) {
    if (error) {
      *error = name + ": partition spec lacks declared bucket fields " +
               bucket_fields.version_field + "/" + bucket_fields.bucket_field;
    }
    return false;
  }
  for (const auto& row : stats.rows) {
    if (row.data_file_count <= 0) continue;
    const int64_t version = row.partition[v_pos];
    if (version > bucket_version) {
      if (error) {
        *error = name + ": partition stats declare bucket-version " +
                 std::to_string(version) + " newer than requested " +
                 std::to_string(bucket_version);
      }
      return false;
    }
    if (version < bucket_version) continue;
    const auto bucket = static_cast<int32_t>(row.partition[b_pos]);
    if (!out->any || bucket > out->frontier) {
      out->any = true;
      out->frontier = bucket;
      out->frontier_bytes = row.total_data_file_size_in_bytes;
      out->frontier_files = row.data_file_count;
    }
  }
  return true;
}

}  // namespace

bool LoadAlignedResume(const std::shared_ptr<iceberg::Catalog>& catalog,
                       const iceberg::Namespace& ns,
                       const std::vector<std::string>& table_names,
                       const std::string& reference_table,
                       const BucketFields& bucket_fields,
                       int32_t bucket_version, ResumeState* out,
                       std::string* error) {
  *out = ResumeState{};

  std::map<std::string, BucketStats> per_table;
  for (const auto& name : table_names) {
    out->next_seq[name] = 0;
    auto t = catalog->LoadTable(iceberg::TableIdentifier{.ns = ns, .name = name});
    if (!t.has_value()) {
      if (t.error().kind == iceberg::ErrorKind::kNoSuchTable) continue;
      if (error) *error = "LoadTable(" + name + "): " + t.error().message;
      return false;
    }
    auto snap_r = t.value()->current_snapshot();
    if (!snap_r.has_value() || !snap_r.value()) continue;
    BucketStats bs;
    if (!BucketStatsForTable(*t.value(), name, bucket_fields, bucket_version,
                             &bs, error)) {
      return false;
    }
    per_table[name] = bs;
  }

  auto ref = per_table.find(reference_table);
  if (ref != per_table.end() && ref->second.any) {
    out->bucket = ref->second.frontier;
    out->bucket_bytes = ref->second.frontier_bytes;
  }
  for (const auto& [name, bs] : per_table) {
    if (!bs.any) continue;
    if (bs.frontier > out->bucket) {
      if (error) {
        *error = name + ": frontier bucket " + std::to_string(bs.frontier) +
                 " is past the reference frontier " + std::to_string(out->bucket);
      }
      return false;
    }
    if (bs.frontier == out->bucket) {
      out->next_seq[name] = bs.frontier_files;
    }
  }
  return true;
}

}  // namespace primeparts
