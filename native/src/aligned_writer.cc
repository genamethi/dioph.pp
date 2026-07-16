#include "primeparts/aligned_writer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory>
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

}  // namespace

struct AlignedBucketWriter::Impl {
  fs::path warehouse;
  iceberg::Namespace ns;
  std::vector<BoundTable> tables;
  AtomKey atom;
  ShapePolicy policy;
  int ref_idx = -1;

  int32_t bucket = 0;
  int64_t bucket_ref_bytes = 0;
  int64_t bucket_base_bytes = 0;
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
  int64_t ref_bytes = 0;
  for (size_t t = 0; t < tables.size(); ++t) {
    if (!writers[t]) continue;
    std::vector<WrittenFile> files;
    if (!writers[t]->Close(&files, error)) return false;
    next_seq[t] = writers[t]->next_file_seq();
    if (static_cast<int>(t) == ref_idx) {
      for (const auto& f : files) ref_bytes += f.bytes;
    }
    all_files[t].insert(all_files[t].end(), files.begin(), files.end());
    writers[t].reset();
  }
  bucket_ref_bytes = bucket_base_bytes + ref_bytes;
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
      bucket_base_bytes = 0;
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
  impl->bucket_base_bytes = resume.bucket_bytes;

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
  out->resume = ResumeState{};
  out->resume.bucket_version = I.policy.bucket_version;
  out->resume.bucket = I.bucket;
  out->resume.bucket_bytes = I.bucket_ref_bytes;
  for (size_t t = 0; t < I.tables.size(); ++t) {
    out->resume.next_seq[I.tables[t].name] = I.next_seq[t];
    out->tables.push_back(TableFiles{I.tables[t].name, std::move(I.all_files[t])});
  }
  return true;
}

std::map<std::string, std::string> AlignedResumeSummary(
    const ResumeState& resume, const std::string& table,
    const std::string& reference_table) {
  std::map<std::string, std::string> out;
  out[kAlignedBucketVersionKey] = std::to_string(resume.bucket_version);
  out[kAlignedBucketKey] = std::to_string(resume.bucket);
  auto it = resume.next_seq.find(table);
  out[kAlignedNextSeqKey] =
      std::to_string(it != resume.next_seq.end() ? it->second : 0);
  if (table == reference_table) {
    out[kAlignedBucketBytesKey] = std::to_string(resume.bucket_bytes);
  }
  return out;
}

namespace {

bool SummaryInt(const std::unordered_map<std::string, std::string>& summary,
                const std::string& table, const char* key, int64_t* out,
                std::string* error) {
  auto it = summary.find(key);
  if (it == summary.end()) {
    if (error) {
      *error = table + ": snapshot summary lacks " + key +
               "; last commit predates aligned-resume declaration";
    }
    return false;
  }
  const auto& s = it->second;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), *out);
  if (ec != std::errc() || ptr != s.data() + s.size()) {
    if (error) *error = table + ": " + key + " is not an integer: " + s;
    return false;
  }
  return true;
}

}  // namespace

bool LoadAlignedResume(const std::shared_ptr<iceberg::Catalog>& catalog,
                       const iceberg::Namespace& ns,
                       const std::vector<std::string>& table_names,
                       const std::string& reference_table,
                       int32_t bucket_version, ResumeState* out,
                       std::string* error) {
  *out = ResumeState{};
  out->bucket_version = bucket_version;

  struct Declared {
    int64_t bucket = 0;
    int64_t next_seq = 0;
    int64_t bucket_bytes = 0;
  };
  std::map<std::string, Declared> decls;

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
    const auto& summary = snap_r.value()->summary;

    int64_t version = 0;
    Declared d;
    if (!SummaryInt(summary, name, kAlignedBucketVersionKey, &version, error) ||
        !SummaryInt(summary, name, kAlignedBucketKey, &d.bucket, error) ||
        !SummaryInt(summary, name, kAlignedNextSeqKey, &d.next_seq, error)) {
      return false;
    }
    if (version > bucket_version) {
      if (error) {
        *error = name + ": declares bucket-version " + std::to_string(version) +
                 " newer than requested " + std::to_string(bucket_version);
      }
      return false;
    }
    if (version < bucket_version) continue;
    if (name == reference_table &&
        !SummaryInt(summary, name, kAlignedBucketBytesKey, &d.bucket_bytes,
                    error)) {
      return false;
    }
    decls[name] = d;
  }

  auto ref = decls.find(reference_table);
  if (ref != decls.end()) {
    out->bucket = static_cast<int32_t>(ref->second.bucket);
    out->bucket_bytes = ref->second.bucket_bytes;
  }
  for (const auto& [name, d] : decls) {
    if (d.bucket > out->bucket) {
      if (error) {
        *error = name + ": declares bucket " + std::to_string(d.bucket) +
                 " past the reference frontier " + std::to_string(out->bucket);
      }
      return false;
    }
    if (d.bucket == out->bucket) {
      out->next_seq[name] = static_cast<int32_t>(d.next_seq);
    }
  }
  return true;
}

}  // namespace primeparts
