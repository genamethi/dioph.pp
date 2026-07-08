#include "primeparts/aligned_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <arrow/api.h>

#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"

#include "primeparts/catalog/pp_iceberg_rest.h"  // StagingDataDir
#include "primeparts/writer.h"

namespace primeparts {

namespace {

constexpr double kEwmaAlpha = 0.3;

// Raw ascending int64 column pointer, or nullptr if absent / not int64.
const int64_t* Int64Col(const arrow::RecordBatch& batch, const std::string& name) {
  auto col = batch.GetColumnByName(name);
  if (!col || col->type_id() != arrow::Type::INT64) return nullptr;
  return static_cast<const arrow::Int64Array&>(*col).raw_values();
}

// First row whose key > cut_key (sorted-ascending key column).
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

  std::vector<std::unique_ptr<BucketParquetWriter>> writers;  // per table, current bucket
  std::vector<std::vector<WrittenFile>> all_files;            // per table, across buckets
  std::vector<int32_t> next_seq;                              // per table, current bucket

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
  for (size_t t = 0; t < tables.size(); ++t) {
    const auto& bt = tables[t];
    WriterConfig cfg;
    cfg.output_dir = catalog::StagingDataDir(warehouse, bt.name) / vdir / bdir;
    cfg.schema = bt.schema;
    cfg.table_name = bt.name;
    cfg.filename_prefix = bt.name;
    cfg.delta_columns = bt.delta_columns;
    cfg.partition_spec = bt.spec;
    cfg.bucket_version = policy.bucket_version;
    cfg.bucket = bucket;
    cfg.target_rows_per_file = 0;         // facade rolls files explicitly
    cfg.max_row_group_rows = INT64_MAX;   // facade cuts row groups explicitly
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
  const int64_t* p = Int64Col(batch, atom.column);
  const int64_t* rank = Int64Col(batch, "prime_rank");
  BucketParquetWriter::BatchStats stats{};
  stats.p_min = p ? p[start] : 0;
  stats.p_max = p ? p[end - 1] : 0;
  stats.rank_min = rank ? rank[start] : 0;
  stats.rank_max = rank ? rank[end - 1] : 0;
  return writers[t]->Write(*slice, stats, error);
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
      next_seq.assign(tables.size(), 0);  // fresh bucket dir starts at seq 0
      if (!OpenBucketWriters(error)) return false;
    }
  }
  rg_ref_bytes_est = 0;
  ref_atoms_in_rg = 0;
  return true;
}

std::unique_ptr<AlignedBucketWriter> AlignedBucketWriter::Make(
    const fs::path& warehouse, std::vector<BoundTable> tables, AtomKey atom,
    ShapePolicy policy, ResumeState resume, std::string* error) {
  auto impl = std::make_unique<Impl>();
  impl->warehouse = warehouse;
  impl->tables = std::move(tables);
  impl->atom = std::move(atom);
  impl->policy = policy;
  impl->ref_bpr =
      policy.ref_bytes_per_row_prior > 0 ? policy.ref_bytes_per_row_prior : 1.1;
  impl->bucket = resume.bucket;
  impl->bucket_ref_bytes = resume.bucket_fill_bytes;

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

  std::vector<int64_t> cursor(I.tables.size(), 0);  // per-table write cursor
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

bool LoadAlignedResume(const fs::path& warehouse,
                       const std::vector<std::string>& table_names,
                       const std::string& reference_table, int32_t bucket_version,
                       ResumeState* out, std::string* error) {
  (void)error;
  *out = ResumeState{};
  const std::string vdir = "p_bucket_version=" + std::to_string(bucket_version);
  const std::string pfx = "p_bucket=";
  for (const auto& name : table_names) {
    const fs::path base = warehouse / "primeparts" / name / "data" / vdir;
    int32_t max_bucket = -1;
    std::error_code ec;
    if (fs::exists(base, ec)) {
      for (auto& e : fs::directory_iterator(base, ec)) {
        if (!e.is_directory()) continue;
        const auto fn = e.path().filename().string();
        if (fn.rfind(pfx, 0) != 0) continue;
        try {
          int32_t b = std::stoi(fn.substr(pfx.size()));
          if (b > max_bucket) max_bucket = b;
        } catch (...) {
        }
      }
    }
    const int32_t bucket_for_seq = max_bucket < 0 ? 0 : max_bucket;
    const fs::path bucket_dir = base / (pfx + std::to_string(bucket_for_seq));
    out->next_seq[name] = NextFileSeq(bucket_dir, name);
    if (name == reference_table) {
      out->bucket = bucket_for_seq;
      int64_t fill = 0;
      if (max_bucket >= 0 && fs::exists(bucket_dir, ec)) {
        for (auto& f : fs::directory_iterator(bucket_dir, ec)) {
          if (f.is_regular_file()) {
            fill += static_cast<int64_t>(fs::file_size(f.path(), ec));
          }
        }
      }
      out->bucket_fill_bytes = fill;
    }
  }
  return true;
}

}  // namespace primeparts
