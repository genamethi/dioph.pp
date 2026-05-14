#include "primeparts/preflight.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "iceberg/arrow/arrow_file_io.h"
#include "iceberg/avro/avro_register.h"
#include "iceberg/file_io.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/parquet/parquet_register.h"
#include "iceberg/snapshot.h"
#include "iceberg/sort_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"

namespace primeparts {

namespace {

constexpr int32_t kP = 1;
constexpr int32_t kPrimesK = 2;
constexpr int32_t kPartitionsMK = 2;

std::string strip_file_scheme(const std::string& uri) {
  if (uri.rfind("file://", 0) == 0) return uri.substr(7);
  return uri;
}

int64_t decode_int64_le(const std::vector<uint8_t>& bytes) {
  int64_t v = 0;
  const size_t n = bytes.size() < 8 ? bytes.size() : 8;
  for (size_t i = 0; i < n; ++i) {
    v |= static_cast<int64_t>(bytes[i]) << (8 * i);
  }
  if (n == 4 && (bytes[3] & 0x80)) {
    v |= ~static_cast<int64_t>(0) << 32;
  }
  return v;
}

void ensure_format_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    iceberg::avro::RegisterAll();
    iceberg::parquet::RegisterAll();
  });
}

bool sqlite_lookup_metadata_location(const fs::path& sqlite_path,
                                     std::string_view ns,
                                     std::string_view tbl,
                                     std::string* out,
                                     std::string* error) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(sqlite_path.c_str(), &db, SQLITE_OPEN_READONLY,
                      nullptr) != SQLITE_OK) {
    if (error) {
      *error = "sqlite3_open_v2 ";
      *error += sqlite_path.string();
      if (db) {
        *error += ": ";
        *error += sqlite3_errmsg(db);
      }
    }
    if (db) sqlite3_close(db);
    return false;
  }
  const char* sql =
      "SELECT metadata_location FROM iceberg_tables "
      "WHERE table_namespace = ?1 AND table_name = ?2 LIMIT 1";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) {
      *error = "sqlite3_prepare_v2: ";
      *error += sqlite3_errmsg(db);
    }
    sqlite3_close(db);
    return false;
  }
  std::string ns_owned(ns), tbl_owned(tbl);
  sqlite3_bind_text(stmt, 1, ns_owned.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, tbl_owned.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* loc = sqlite3_column_text(stmt, 0);
    if (loc) {
      *out = strip_file_scheme(reinterpret_cast<const char*>(loc));
      ok = !out->empty();
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (!ok && error) {
    *error = "no iceberg_tables row for ";
    *error += ns;
    *error += '.';
    *error += tbl;
  }
  return ok;
}

struct FileInfo {
  std::string path;
  int64_t record_count;
  int64_t p_min;
  int64_t p_max;
};

struct TableInfo {
  std::shared_ptr<iceberg::FileIO> io;
  std::shared_ptr<iceberg::TableMetadata> metadata;
  std::vector<FileInfo> files;  // in manifest enumeration order
  int64_t total_records = 0;
};

bool load_table_info(const fs::path& sqlite_path, std::string_view ns,
                     std::string_view tbl, TableInfo* out,
                     std::string* error) {
  ensure_format_registered();

  std::string metadata_path;
  if (!sqlite_lookup_metadata_location(sqlite_path, ns, tbl, &metadata_path,
                                       error)) {
    return false;
  }

  auto unique_io = iceberg::arrow::MakeLocalFileIO();
  out->io = std::shared_ptr<iceberg::FileIO>(std::move(unique_io));

  auto md_r = iceberg::TableMetadataUtil::Read(*out->io, metadata_path);
  if (!md_r.has_value()) {
    if (error) *error = "TableMetadata::Read " + metadata_path + ": " +
                        md_r.error().message;
    return false;
  }
  out->metadata = std::shared_ptr<iceberg::TableMetadata>(std::move(md_r.value()));

  // Enumerate data files via TableScan::PlanFiles — same path
  // source_scan.cc uses. Each FileScanTask exposes data_file() with
  // record_count and lower/upper_bounds.
  auto builder_r = iceberg::TableScanBuilder<iceberg::DataTableScan>::Make(
      out->metadata, out->io);
  if (!builder_r.has_value()) {
    if (error) *error = "TableScanBuilder::Make: " + builder_r.error().message;
    return false;
  }
  auto scan_r = builder_r.value()->Select({"p"}).IncludeColumnStats({"p"}).Build();
  if (!scan_r.has_value()) {
    if (error) *error = "TableScanBuilder::Build: " + scan_r.error().message;
    return false;
  }
  auto tasks_r = scan_r.value()->PlanFiles();
  if (!tasks_r.has_value()) {
    if (error) *error = "scan->PlanFiles: " + tasks_r.error().message;
    return false;
  }
  for (const auto& task : tasks_r.value()) {
    const auto& df = task->data_file();
    if (!df) continue;
    auto lb = df->lower_bounds.find(kP);
    auto ub = df->upper_bounds.find(kP);
    if (lb == df->lower_bounds.end() || ub == df->upper_bounds.end()) {
      if (error) *error = "data file missing p bounds: " + df->file_path;
      return false;
    }
    FileInfo fi;
    fi.path = strip_file_scheme(df->file_path);
    fi.record_count = df->record_count;
    fi.p_min = decode_int64_le(lb->second);
    fi.p_max = decode_int64_le(ub->second);
    out->total_records += df->record_count;
    out->files.push_back(std::move(fi));
  }

  return true;
}

PreflightCheck check_no_overlap(const TableInfo& info, const std::string& label) {
  PreflightCheck c{label + ": no p-range overlap across files", true, ""};
  std::vector<FileInfo> sorted = info.files;
  std::sort(sorted.begin(), sorted.end(),
            [](const FileInfo& a, const FileInfo& b) { return a.p_min < b.p_min; });

  std::vector<std::string> strict_overlaps;
  size_t touching = 0;
  for (size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].p_min < sorted[i - 1].p_max) {
      std::ostringstream os;
      os << "  [" << sorted[i - 1].p_min << "," << sorted[i - 1].p_max << "] "
         << sorted[i - 1].path << "\n  vs\n  [" << sorted[i].p_min << ","
         << sorted[i].p_max << "] " << sorted[i].path;
      strict_overlaps.push_back(os.str());
    } else if (sorted[i].p_min == sorted[i - 1].p_max) {
      ++touching;
    }
  }
  std::ostringstream detail;
  if (touching > 0) {
    detail << touching << " boundary-touching pair(s) [OK — adjacent files share one prime]\n";
  }
  if (!strict_overlaps.empty()) {
    c.passed = false;
    detail << strict_overlaps.size() << " strictly-overlapping pair(s):\n";
    for (size_t i = 0; i < strict_overlaps.size() && i < 5; ++i)
      detail << strict_overlaps[i] << "\n";
    if (strict_overlaps.size() > 5)
      detail << "  ... and " << strict_overlaps.size() - 5 << " more\n";
  }
  c.detail = detail.str();
  return c;
}

PreflightCheck check_sort_order(const TableInfo& info, const std::string& label,
                                const std::vector<int32_t>& expected_field_ids) {
  PreflightCheck c{label + ": sort order metadata", true, ""};
  auto so_r = info.metadata->SortOrderById(info.metadata->default_sort_order_id);
  if (!so_r.has_value()) {
    c.passed = false;
    c.detail = "SortOrderById: " + so_r.error().message;
    return c;
  }
  const auto& so = *so_r.value();
  if (!so.is_sorted()) {
    c.passed = false;
    c.detail = "table is unsorted (sort_order_id = " +
               std::to_string(so.order_id()) + ")";
    return c;
  }
  auto fields = so.fields();
  if (fields.size() != expected_field_ids.size()) {
    c.passed = false;
    std::ostringstream os;
    os << "expected " << expected_field_ids.size() << " sort fields, got "
       << fields.size();
    c.detail = os.str();
    return c;
  }
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].source_id() != expected_field_ids[i]) {
      std::ostringstream os;
      os << "sort field " << i << ": expected source_id "
         << expected_field_ids[i] << ", got " << fields[i].source_id();
      c.passed = false;
      c.detail = os.str();
      return c;
    }
    if (fields[i].direction() != iceberg::SortDirection::kAscending) {
      std::ostringstream os;
      os << "sort field " << i << ": expected ascending, got "
         << iceberg::ToString(fields[i].direction());
      c.passed = false;
      c.detail = os.str();
      return c;
    }
  }
  return c;
}

// Read a column from a parquet file, return a single Int64Array. If the
// source column is int32 it is promoted to int64 on read. Returns
// nullptr on error.
std::shared_ptr<arrow::Int64Array> read_int64_column(const std::string& path,
                                                    const std::string& column,
                                                    std::string* error) {
  auto file_r = arrow::io::ReadableFile::Open(path);
  if (!file_r.ok()) {
    if (error) *error = "open " + path + ": " + file_r.status().ToString();
    return nullptr;
  }
  auto reader_r = parquet::arrow::OpenFile(file_r.ValueOrDie(),
                                           arrow::default_memory_pool());
  if (!reader_r.ok()) {
    if (error) *error = "parquet::OpenFile " + path + ": " +
                        reader_r.status().ToString();
    return nullptr;
  }
  auto reader = std::move(reader_r).ValueOrDie();
  std::shared_ptr<arrow::Schema> schema;
  auto st = reader->GetSchema(&schema);
  if (!st.ok()) {
    if (error) *error = "GetSchema: " + st.ToString();
    return nullptr;
  }
  int col_idx = schema->GetFieldIndex(column);
  if (col_idx < 0) {
    if (error) *error = "column '" + column + "' not found in " + path;
    return nullptr;
  }
  std::shared_ptr<arrow::ChunkedArray> col;
  st = reader->ReadColumn(col_idx, &col);
  if (!st.ok()) {
    if (error) *error = "ReadColumn(" + column + "): " + st.ToString();
    return nullptr;
  }
  // Build a single Int64Array. Source may be int32 (k, m_k, n_k) or
  // int64 (p, q_k). Allocate output, fill from chunks promoting as
  // needed.
  arrow::Int64Builder builder;
  if (!builder.Reserve(col->length()).ok()) {
    if (error) *error = "Reserve";
    return nullptr;
  }
  for (const auto& chunk : col->chunks()) {
    if (chunk->type_id() == arrow::Type::INT64) {
      const auto& a = static_cast<const arrow::Int64Array&>(*chunk);
      const int64_t* v = a.raw_values();
      for (int64_t i = 0; i < a.length(); ++i) builder.UnsafeAppend(v[i]);
    } else if (chunk->type_id() == arrow::Type::INT32) {
      const auto& a = static_cast<const arrow::Int32Array&>(*chunk);
      const int32_t* v = a.raw_values();
      for (int64_t i = 0; i < a.length(); ++i)
        builder.UnsafeAppend(static_cast<int64_t>(v[i]));
    } else {
      if (error)
        *error = "column '" + column + "' unexpected type: " + chunk->type()->ToString();
      return nullptr;
    }
  }
  std::shared_ptr<arrow::Array> out;
  st = builder.Finish(&out);
  if (!st.ok()) {
    if (error) *error = "Finish: " + st.ToString();
    return nullptr;
  }
  return std::static_pointer_cast<arrow::Int64Array>(out);
}

// Streaming sort check for one parquet file. Reads (p) or (p, m_k) via
// GetRecordBatchReader so memory stays bounded to one batch.
// `has_mk`=true means partitions table: enforce (p ASC, m_k ASC).
// Returns empty string on pass, failure description on fail.
std::string scan_file_sort(const std::string& path, bool has_mk) {
  auto file_r = arrow::io::ReadableFile::Open(path);
  if (!file_r.ok()) return "open: " + file_r.status().ToString();
  auto reader_r = parquet::arrow::OpenFile(file_r.ValueOrDie(),
                                           arrow::default_memory_pool());
  if (!reader_r.ok()) return "OpenFile: " + reader_r.status().ToString();
  auto reader = std::move(reader_r).ValueOrDie();
  std::shared_ptr<arrow::Schema> schema;
  auto st = reader->GetSchema(&schema);
  if (!st.ok()) return "GetSchema: " + st.ToString();

  int p_idx = schema->GetFieldIndex("p");
  if (p_idx < 0) return "column 'p' not found";
  int mk_idx = has_mk ? schema->GetFieldIndex("m_k") : -1;
  if (has_mk && mk_idx < 0) return "column 'm_k' not found";

  std::vector<int> cols = has_mk ? std::vector<int>{p_idx, mk_idx}
                                 : std::vector<int>{p_idx};
  std::vector<int> rgs(reader->num_row_groups());
  for (size_t i = 0; i < rgs.size(); ++i) rgs[i] = static_cast<int>(i);

  auto rbr_r = reader->GetRecordBatchReader(rgs, cols);
  if (!rbr_r.ok()) return "GetRecordBatchReader: " + rbr_r.status().ToString();
  auto rbr = std::move(rbr_r).ValueOrDie();

  int64_t last_p = std::numeric_limits<int64_t>::min();
  int32_t last_mk = std::numeric_limits<int32_t>::min();
  bool have_last = false;
  int64_t row_offset = 0;

  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    st = rbr->ReadNext(&batch);
    if (!st.ok()) return "ReadNext: " + st.ToString();
    if (!batch) break;

    const int64_t n = batch->num_rows();
    if (n == 0) continue;

    const auto& p_arr = static_cast<const arrow::Int64Array&>(*batch->column(0));
    const int64_t* pv = p_arr.raw_values();

    if (!has_mk) {
      // Primes: strictly ascending (no duplicate p values).
      for (int64_t i = 0; i < n; ++i) {
        const int64_t cur = pv[i];
        if (have_last && cur <= last_p) {
          std::ostringstream os;
          os << "p not strictly ascending at row " << (row_offset + i)
             << " (prev p=" << last_p << ", p=" << cur << ")";
          return os.str();
        }
        last_p = cur;
        have_last = true;
      }
    } else {
      const auto& mk_arr =
          static_cast<const arrow::Int32Array&>(*batch->column(1));
      const int32_t* mv = mk_arr.raw_values();
      for (int64_t i = 0; i < n; ++i) {
        const int64_t cur_p = pv[i];
        const int32_t cur_mk = mv[i];
        if (have_last) {
          if (cur_p < last_p) {
            std::ostringstream os;
            os << "(p, m_k) not ascending at row " << (row_offset + i)
               << ": p went from " << last_p << " to " << cur_p;
            return os.str();
          }
          if (cur_p == last_p && cur_mk <= last_mk) {
            std::ostringstream os;
            os << "(p, m_k) not ascending at row " << (row_offset + i)
               << " (p=" << cur_p << "; prev m_k=" << last_mk
               << ", m_k=" << cur_mk << ")";
            return os.str();
          }
        }
        last_p = cur_p;
        last_mk = cur_mk;
        have_last = true;
      }
    }
    row_offset += n;
  }
  return "";  // clean
}

// Run scan_file_sort on every file with N parallel workers.
PreflightCheck check_all_files_sort(const TableInfo& info, bool has_mk,
                                    const std::string& label,
                                    size_t num_threads) {
  PreflightCheck c{label, true, ""};
  if (info.files.empty()) { c.passed = false; c.detail = "no files"; return c; }

  std::atomic<size_t> next_idx{0};
  std::mutex out_mu;
  std::vector<std::pair<std::string, std::string>> failures;  // {path, detail}

  auto worker = [&]() {
    while (true) {
      size_t i = next_idx.fetch_add(1);
      if (i >= info.files.size()) return;
      const auto& fi = info.files[i];
      std::string fail = scan_file_sort(fi.path, has_mk);
      if (!fail.empty()) {
        std::lock_guard<std::mutex> g(out_mu);
        failures.push_back({fi.path, std::move(fail)});
      }
    }
  };

  std::vector<std::thread> threads;
  for (size_t t = 0; t < num_threads; ++t) threads.emplace_back(worker);
  for (auto& th : threads) th.join();

  if (!failures.empty()) {
    c.passed = false;
    std::sort(failures.begin(), failures.end());
    std::ostringstream os;
    os << failures.size() << " of " << info.files.size()
       << " files failed sort check:\n";
    const size_t show = std::min<size_t>(failures.size(), 10);
    for (size_t i = 0; i < show; ++i) {
      os << "  " << failures[i].first << ": " << failures[i].second << "\n";
    }
    if (failures.size() > show)
      os << "  ... and " << failures.size() - show << " more\n";
    c.detail = os.str();
  } else {
    std::ostringstream os;
    os << info.files.size() << " files scanned";
    c.detail = os.str();
  }
  return c;
}

// Sum the k column across all primes data files. We open each file
// sequentially, read just the k column, and accumulate the SIMD sum.
// Bounded memory: one file's k column at a time.
PreflightCheck check_k_sum(const TableInfo& primes, const TableInfo& partitions) {
  PreflightCheck c{"primes.k sum == partitions.record_count sum", true, ""};
  int64_t k_sum = 0;
  // Stream row-group-at-a-time to bound memory. Reading the entire k
  // column at once is ~3GB per file × 57 files; row groups are
  // typically 64-256MB each so this stays well within RAM.
  for (const auto& fi : primes.files) {
    std::string err;
    auto k = read_int64_column(fi.path, "k", &err);
    if (!k) {
      c.passed = false;
      c.detail = "reading k from " + fi.path + ": " + err;
      return c;
    }
    const int64_t* v = k->raw_values();
    const int64_t n = k->length();
    int64_t partial = 0;
    for (int64_t i = 0; i < n; ++i) partial += v[i];
    k_sum += partial;
  }
  if (k_sum != partitions.total_records) {
    c.passed = false;
    std::ostringstream os;
    os << "k_sum=" << k_sum << " != partitions.total_records="
       << partitions.total_records << " (diff "
       << (k_sum - partitions.total_records) << ")";
    c.detail = os.str();
  }
  return c;
}

}  // namespace

std::string PreflightReport::ToJson() const {
  nlohmann::json j;
  j["checks"] = nlohmann::json::array();
  for (const auto& c : checks) {
    j["checks"].push_back({{"name", c.name},
                           {"passed", c.passed},
                           {"detail", c.detail}});
  }
  j["all_passed"] = all_passed();
  return j.dump(2);
}

PreflightReport RunPreflight(const fs::path& sqlite_path,
                             std::string_view ns,
                             std::string_view primes_tbl,
                             std::string_view partitions_tbl) {
  PreflightReport report;

  TableInfo primes, partitions;
  std::string err;

  const std::string primes_label = std::string(ns) + "." + std::string(primes_tbl);
  const std::string partitions_label = std::string(ns) + "." + std::string(partitions_tbl);

  if (!load_table_info(sqlite_path, ns, primes_tbl, &primes, &err)) {
    report.checks.push_back({"load " + primes_label + " metadata", false, err});
    return report;
  }
  report.checks.push_back({"load " + primes_label + " metadata", true,
                           std::to_string(primes.files.size()) + " files, " +
                               std::to_string(primes.total_records) + " rows"});

  if (!load_table_info(sqlite_path, ns, partitions_tbl, &partitions, &err)) {
    report.checks.push_back({"load " + partitions_label + " metadata", false, err});
    return report;
  }
  report.checks.push_back(
      {"load " + partitions_label + " metadata", true,
       std::to_string(partitions.files.size()) + " files, " +
           std::to_string(partitions.total_records) + " rows"});

  // Check 1: no overlap (both tables).
  report.checks.push_back(check_no_overlap(primes, primes_label));
  report.checks.push_back(check_no_overlap(partitions, partitions_label));

  // Check 2: sort orders.
  report.checks.push_back(check_sort_order(primes, primes_label, {kP}));
  report.checks.push_back(
      check_sort_order(partitions, partitions_label, {kP, kPartitionsMK}));

  // Check 3: in-file sort, ALL files, parallel.
  const size_t nthreads = 12;
  report.checks.push_back(check_all_files_sort(
      primes, /*has_mk=*/false,
      primes_label + ": in-file p sort (all files)", nthreads));
  report.checks.push_back(check_all_files_sort(
      partitions, /*has_mk=*/true,
      partitions_label + ": in-file (p, m_k) sort (all files)", nthreads));

  // Check 4: k-sum.
  report.checks.push_back(check_k_sum(primes, partitions));

  return report;
}

}  // namespace primeparts
