#include "primeparts/aligned_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <arrow/api.h>

#include "iceberg/catalog.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/partition_field.h"
#include "iceberg/partition_spec.h"
#include "iceberg/result.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
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
    cfg.output_dir = catalog::StagingDataDir(warehouse, bt.name) / vdir / bdir;
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

int32_t SeqFromFilename(const std::string& name) {
  auto pos = name.rfind('_');
  if (pos == std::string::npos) return -1;
  auto dot = name.find('.', pos);
  if (dot == std::string::npos) return -1;
  try {
    return static_cast<int32_t>(std::stoi(name.substr(pos + 1, dot - pos - 1)));
  } catch (...) {
    return -1;
  }
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
  for (const auto& name : table_names) {
    out->next_seq[name] = 0;
    auto t = catalog->LoadTable(iceberg::TableIdentifier{.ns = ns, .name = name});
    if (!t.has_value()) {
      if (t.error().kind == iceberg::ErrorKind::kNoSuchTable) continue;
      if (error) *error = "LoadTable(" + name + "): " + t.error().message;
      return false;
    }
    auto tbl = t.value();
    auto snap_r = tbl->current_snapshot();
    if (!snap_r.has_value() || !snap_r.value()) continue;
    auto schema_r = tbl->schema();
    if (!schema_r.has_value()) {
      if (error) *error = name + ": schema: " + schema_r.error().message;
      return false;
    }
    auto spec_r = tbl->spec();
    if (!spec_r.has_value()) {
      if (error) *error = name + ": spec: " + spec_r.error().message;
      return false;
    }
    const auto& spec = spec_r.value();

    int v_pos = -1;
    int b_pos = -1;
    const auto spec_fields = spec->fields();
    for (size_t i = 0; i < spec_fields.size(); ++i) {
      if (spec_fields[i].name() == bucket_fields.version_field) {
        v_pos = static_cast<int>(i);
      } else if (spec_fields[i].name() == bucket_fields.bucket_field) {
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

    iceberg::SnapshotCache cache(snap_r.value().get());
    auto manifests = cache.DataManifests(tbl->io());
    if (!manifests.has_value()) {
      if (error) *error = name + ": manifests: " + manifests.error().message;
      return false;
    }

    struct CommittedFile {
      int32_t bucket;
      int64_t bytes;
      std::string base;
    };
    std::vector<CommittedFile> files;
    int32_t max_bucket = -1;
    for (const auto& m : manifests.value()) {
      auto reader = iceberg::ManifestReader::Make(m, tbl->io(), schema_r.value(),
                                                  spec);
      if (!reader.has_value()) {
        if (error) *error = name + ": manifest: " + reader.error().message;
        return false;
      }
      auto entries = reader.value()->LiveEntries();
      if (!entries.has_value()) {
        if (error) *error = name + ": entries: " + entries.error().message;
        return false;
      }
      for (const auto& entry : entries.value()) {
        const auto& df = entry.data_file;
        if (!df) continue;
        auto v_lit = df->partition.ValueAt(static_cast<size_t>(v_pos));
        auto b_lit = df->partition.ValueAt(static_cast<size_t>(b_pos));
        if (!v_lit.has_value() || !b_lit.has_value()) {
          if (error) {
            *error = name + ": partition tuple of " + df->file_path +
                     " lacks the bucket fields";
          }
          return false;
        }
        const auto* v_val =
            std::get_if<int32_t>(&v_lit.value().get().value());
        const auto* b_val =
            std::get_if<int32_t>(&b_lit.value().get().value());
        if (!v_val || !b_val) {
          if (error) {
            *error = name + ": bucket partition values of " + df->file_path +
                     " are not int32";
          }
          return false;
        }
        if (*v_val != bucket_version) continue;
        if (*b_val > max_bucket) max_bucket = *b_val;
        files.push_back(CommittedFile{
            *b_val, df->file_size_in_bytes,
            fs::path(df->file_path).filename().string()});
      }
    }

    const int32_t frontier = max_bucket < 0 ? 0 : max_bucket;
    int32_t max_seq = -1;
    int64_t fill = 0;
    for (const auto& f : files) {
      if (f.bucket != frontier) continue;
      const int32_t seq = SeqFromFilename(f.base);
      if (seq < 0) {
        if (error) {
          *error = name + ": committed file has no parseable sequence: " + f.base;
        }
        return false;
      }
      if (seq > max_seq) max_seq = seq;
      fill += f.bytes;
    }
    out->next_seq[name] = max_seq + 1;
    if (name == reference_table) {
      out->bucket = frontier;
      out->bucket_fill_bytes = max_bucket < 0 ? 0 : fill;
    }
  }
  return true;
}

}  // namespace primeparts
