// primeparts/catalog/pp_delete_spike.cc — see header.

#include "primeparts/catalog/pp_delete_spike.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <arrow/api.h>

#include "primeparts/catalog/pp_hive_sync.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/pp_row_delta.h"
#include "primeparts/source_scan.h"

#include "iceberg/catalog.h"
#include "iceberg/data/position_delete_writer.h"
#include "iceberg/file_format.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_scan.h"

namespace primeparts::catalog {

namespace fs = std::filesystem;

namespace {

// Strip a leading file: scheme (iceberg locations are file:/abs/path).
std::string StripFileScheme(std::string_view uri) {
  constexpr std::string_view kPfx = "file:";
  if (uri.substr(0, kPfx.size()) == kPfx) uri.remove_prefix(kPfx.size());
  return std::string(uri);
}

// Count rows via beeline (the engine applies position deletes on read). This
// is also how the sieve's consumers will see the post-delete set. Returns -1
// on error; parses the single integer beeline prints for COUNT(*).
int64_t CountViaBeeline(const HiveSyncOptions& hopts, const std::string& db_table,
                        std::string* error) {
  std::string out;
  if (!HiveExec(hopts, "SELECT COUNT(*) FROM " + db_table, &out)) {
    *error = out;
    return -1;
  }
  // beeline tsv2 output: the count is the last all-digits token in `out`.
  int64_t val = -1;
  std::string cur;
  auto try_parse = [&](const std::string& tok) {
    if (tok.empty()) return;
    for (char c : tok) if (c < '0' || c > '9') return;
    val = std::stoll(tok);
  };
  for (char c : out) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { try_parse(cur); cur.clear(); }
    else cur.push_back(c);
  }
  try_parse(cur);
  if (val < 0) *error = "could not parse COUNT(*) from: " + out;
  return val;
}

}  // namespace

int RunDeleteSpike(const DeleteSpikeOptions& opts) {
  std::printf("== pp-catalog delete-primitive spike ==\n");
  std::printf("  rest-uri : %s\n", opts.rest_uri.c_str());
  std::printf("  warehouse: %s\n", opts.warehouse.c_str());

  char name[80];
  std::snprintf(name, sizeof(name), "zz_ppcatalog_delspike_%ld",
                static_cast<long>(std::time(nullptr)));
  const std::string table = name;
  const std::string db_table = "primeparts." + table;

  HiveSyncOptions hopts;
  std::string out, err;

  // --- Seed a throwaway table with 5 known rows, via beeline ---------------
  std::printf("\n[1/6] create + seed %s (5 rows) ... ", db_table.c_str());
  if (!HiveExec(hopts,
        "DROP TABLE IF EXISTS " + db_table + "; "
        "CREATE TABLE " + db_table + " (id bigint) STORED BY ICEBERG "
        "STORED AS PARQUET TBLPROPERTIES ('format-version'='2'); "
        "INSERT INTO " + db_table + " VALUES (10),(20),(30),(40),(50)",
        &out)) {
    std::printf("FAIL\n%s\n", out.c_str());
    return 1;
  }
  std::printf("OK\n");

  auto cleanup = [&](int rc) -> int {
    if (opts.keep) {
      std::printf("[--keep] leaving %s\n", db_table.c_str());
    } else {
      std::string o;
      HiveExec(hopts, "DROP TABLE IF EXISTS " + db_table, &o);
    }
    std::printf("\n== delete spike %s ==\n", rc == 0 ? "PASSED" : "FAILED");
    return rc;
  };

  // --- Load via IRC --------------------------------------------------------
  std::printf("[2/6] IRC load %s ... ", db_table.c_str());
  std::string mode;
  RestOptions ropts;
  ropts.rest_uri = opts.rest_uri;
  auto catalog = MakeCatalog(ropts, opts.warehouse, &mode, &err);
  if (!catalog) { std::printf("FAIL (MakeCatalog: %s)\n", err.c_str()); return cleanup(1); }
  iceberg::TableIdentifier ident{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = table};
  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    std::printf("FAIL (LoadTable: %s)\n", loaded.error().message.c_str());
    return cleanup(1);
  }
  auto tbl = loaded.value();
  std::printf("OK\n");

  // --- Baseline count (via beeline) ----------------------------------------
  std::printf("[3/6] baseline count ... ");
  int64_t base_count = CountViaBeeline(hopts, db_table, &err);
  if (base_count < 0) { std::printf("FAIL (%s)\n", err.c_str()); return cleanup(1); }
  std::printf("%lld\n", static_cast<long long>(base_count));
  if (base_count != 5) {
    std::printf("      (expected 5; aborting)\n");
    return cleanup(1);
  }

  // --- Plan files to get the data file path + spec -------------------------
  std::printf("[4/6] write position-delete file (delete 2 rows) ... ");
  auto scan_b = tbl->NewScan();
  if (!scan_b.has_value()) { std::printf("FAIL (NewScan: %s)\n", scan_b.error().message.c_str()); return cleanup(1); }
  auto scan = scan_b.value()->Build();
  if (!scan.has_value()) { std::printf("FAIL (Scan Build: %s)\n", scan.error().message.c_str()); return cleanup(1); }
  auto tasks = scan.value()->PlanFiles();
  if (!tasks.has_value()) {
    std::printf("FAIL (PlanFiles: %s)\n", tasks.error().message.c_str()); return cleanup(1);
  }
  if (tasks.value().empty()) {
    std::printf("FAIL (PlanFiles: no tasks)\n"); return cleanup(1);
  }
  auto data_file = tasks.value().front()->data_file();
  const std::string data_path = data_file->file_path;

  // Write a position-delete file deleting positions 0 and 2 (ids 10 and 30).
  auto schema_r = tbl->schema();
  auto spec_r = tbl->spec();
  if (!schema_r.has_value() || !spec_r.has_value()) {
    std::printf("FAIL (schema/spec)\n"); return cleanup(1);
  }
  const std::string del_path =
      StripFileScheme(tbl->location()) + "/data/delete-spike-" + name + ".parquet";
  iceberg::PositionDeleteWriterOptions wopts{
      .path = del_path,
      .schema = schema_r.value(),
      .spec = spec_r.value(),
      .partition = iceberg::PartitionValues{},
      .format = iceberg::FileFormatType::kParquet,
      .io = tbl->io(),
      .flush_threshold = 10000,
      .properties = {{"write.parquet.compression-codec", "zstd"}},
  };
  auto writer_r = iceberg::PositionDeleteWriter::Make(wopts);
  if (!writer_r.has_value()) { std::printf("FAIL (writer: %s)\n", writer_r.error().message.c_str()); return cleanup(1); }
  auto writer = std::move(writer_r.value());
  if (auto s = writer->WriteDelete(data_path, 0); !s.has_value()) { std::printf("FAIL (WriteDelete 0)\n"); return cleanup(1); }
  if (auto s = writer->WriteDelete(data_path, 2); !s.has_value()) { std::printf("FAIL (WriteDelete 2)\n"); return cleanup(1); }
  if (auto s = writer->Close(); !s.has_value()) { std::printf("FAIL (Close: %s)\n", s.error().message.c_str()); return cleanup(1); }
  auto meta_r = writer->Metadata();
  if (!meta_r.has_value() || meta_r.value().data_files.empty()) { std::printf("FAIL (Metadata)\n"); return cleanup(1); }
  auto delete_file = meta_r.value().data_files.front();
  std::printf("OK (%s)\n", fs::path(del_path).filename().c_str());

  // --- Commit the delete via RowDelta --------------------------------------
  std::printf("[5/6] commit RowDelta ... ");
  auto rd_r = RowDelta::Make(tbl);
  if (!rd_r.has_value()) { std::printf("FAIL (RowDelta::Make: %s)\n", rd_r.error().message.c_str()); return cleanup(1); }
  auto rd = rd_r.value();
  rd->AddDeleteFile(delete_file);
  if (auto s = rd->Commit(); !s.has_value()) {
    std::printf("FAIL (Commit: %s)\n", s.error().message.c_str());
    return cleanup(1);
  }
  std::printf("OK\n");

  // --- Reopen + read back --------------------------------------------------
  // The RowDelta commit advanced the snapshot via IRC; for the Hive engine to
  // see it we run the verified HMS sync (ALTER metadata_location via beeline),
  // pointing at the new metadata.json. Then COUNT(*) reflects the deletes.
  std::printf("[6/6] HMS-sync new snapshot + read back ... ");
  auto reloaded = catalog->LoadTable(ident);
  if (!reloaded.has_value()) { std::printf("FAIL (reload: %s)\n", reloaded.error().message.c_str()); return cleanup(1); }
  const std::string new_md_uri(reloaded.value()->metadata_file_location());
  if (!HiveSync(hopts, db_table, new_md_uri, &out)) {
    std::printf("FAIL (HiveSync: %s)\n", out.c_str()); return cleanup(1);
  }
  int64_t after = CountViaBeeline(hopts, db_table, &err);
  if (after < 0) { std::printf("FAIL (%s)\n", err.c_str()); return cleanup(1); }
  std::printf("%lld\n", static_cast<long long>(after));

  std::printf("\n  baseline=%lld  after-delete=%lld  expected=3\n",
              static_cast<long long>(base_count), static_cast<long long>(after));
  return cleanup(after == 3 ? 0 : 1);
}

namespace {

// One live row, as the sieve will see it: the prime, plus the
// (data file path, absolute ordinal) a position delete needs.
struct LiveRow {
  int64_t p = 0;
  std::string file;
  int64_t pos = 0;
};

// Read the table at `metadata_path` natively (no beeline), MOR-aware,
// projecting [p, _pos]. Returns live rows with their absolute positions.
// On failure returns false and sets *error.
bool ReadLiveRows(const std::string& metadata_path, std::vector<LiveRow>* out,
                  std::string* error) {
  out->clear();
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      metadata_path, {"p", "_pos"}, /*filter=*/nullptr, error);
  if (!reader) return false;
  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (!reader->Next(&batch, error)) return false;
    if (!batch) break;  // EOF
    auto p_col = batch->GetColumnByName("p");
    auto pos_col = batch->GetColumnByName("_pos");
    if (!p_col || !pos_col) {
      *error = "projection did not return [p, _pos] (got " +
               batch->schema()->ToString() + ")";
      return false;
    }
    auto p_arr = std::static_pointer_cast<arrow::Int64Array>(p_col);
    auto pos_arr = std::static_pointer_cast<arrow::Int64Array>(pos_col);
    const std::string& file = reader->current_data_file_path();
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      out->push_back(LiveRow{p_arr->Value(i), file, pos_arr->Value(i)});
    }
  }
  return true;
}

// Write a position-delete file removing the given (file,pos) rows, then commit
// it via RowDelta over IRC. Returns commit wall-time in ms (-1 on failure).
double DeleteAndCommit(const std::shared_ptr<iceberg::Table>& tbl,
                       const std::string& del_path,
                       const std::vector<LiveRow>& targets, std::string* error) {
  auto schema_r = tbl->schema();
  auto spec_r = tbl->spec();
  if (!schema_r.has_value() || !spec_r.has_value()) {
    *error = "schema/spec unavailable";
    return -1;
  }
  iceberg::PositionDeleteWriterOptions wopts{
      .path = del_path,
      .schema = schema_r.value(),
      .spec = spec_r.value(),
      .partition = iceberg::PartitionValues{},
      .format = iceberg::FileFormatType::kParquet,
      .io = tbl->io(),
      .flush_threshold = 10000,
      .properties = {{"write.parquet.compression-codec", "zstd"}},
  };
  auto writer_r = iceberg::PositionDeleteWriter::Make(wopts);
  if (!writer_r.has_value()) { *error = writer_r.error().message; return -1; }
  auto writer = std::move(writer_r.value());
  // Position deletes must be sorted by (file_path, pos).
  std::vector<LiveRow> sorted = targets;
  std::sort(sorted.begin(), sorted.end(), [](const LiveRow& a, const LiveRow& b) {
    return a.file != b.file ? a.file < b.file : a.pos < b.pos;
  });
  for (const auto& r : sorted) {
    if (auto s = writer->WriteDelete(r.file, r.pos); !s.has_value()) {
      *error = "WriteDelete: " + s.error().message;
      return -1;
    }
  }
  if (auto s = writer->Close(); !s.has_value()) { *error = s.error().message; return -1; }
  auto meta_r = writer->Metadata();
  if (!meta_r.has_value() || meta_r.value().data_files.empty()) {
    *error = "writer Metadata empty";
    return -1;
  }
  auto rd_r = primeparts::catalog::RowDelta::Make(tbl);
  if (!rd_r.has_value()) { *error = rd_r.error().message; return -1; }
  auto rd = rd_r.value();
  rd->AddDeleteFile(meta_r.value().data_files.front());
  auto t0 = std::chrono::steady_clock::now();
  auto cm = rd->Commit();
  auto t1 = std::chrono::steady_clock::now();
  if (!cm.has_value()) { *error = cm.error().message; return -1; }
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int RunMorVerify(const DeleteSpikeOptions& opts) {
  std::printf("== pp-catalog MOR-verify (native read-back, no beeline) ==\n");
  std::printf("  rest-uri : %s\n", opts.rest_uri.c_str());
  std::printf("  warehouse: %s\n", opts.warehouse.c_str());

  char name[80];
  std::snprintf(name, sizeof(name), "zz_ppcatalog_morverify_%ld",
                static_cast<long>(std::time(nullptr)));
  const std::string table = name;
  const std::string db_table = "primeparts." + table;
  constexpr int kRows = 10;

  HiveSyncOptions hopts;
  std::string out, err;

  // --- Seed a throwaway v2 (p, prime_rank) table -----------------------------
  std::printf("\n[1/5] create + seed %s (%d rows) ... ", db_table.c_str(), kRows);
  if (!HiveExec(hopts,
        "DROP TABLE IF EXISTS " + db_table + "; "
        "CREATE TABLE " + db_table + " (p bigint, prime_rank bigint) "
        "STORED BY ICEBERG STORED AS PARQUET TBLPROPERTIES ('format-version'='2'); "
        "INSERT INTO " + db_table + " VALUES "
        "(3,2),(5,3),(7,4),(11,5),(13,6),(17,7),(19,8),(23,9),(29,10),(31,11)",
        &out)) {
    std::printf("FAIL\n%s\n", out.c_str());
    return 1;
  }
  std::printf("OK\n");

  auto cleanup = [&](int rc) -> int {
    if (opts.keep) {
      std::printf("[--keep] leaving %s\n", db_table.c_str());
    } else {
      std::string o;
      HiveExec(hopts, "DROP TABLE IF EXISTS " + db_table, &o);
    }
    std::printf("\n== MOR-verify %s ==\n", rc == 0 ? "PASSED" : "FAILED");
    return rc;
  };

  // --- IRC load --------------------------------------------------------------
  std::printf("[2/5] IRC load ... ");
  std::string mode;
  RestOptions ropts;
  ropts.rest_uri = opts.rest_uri;
  auto catalog = MakeCatalog(ropts, opts.warehouse, &mode, &err);
  if (!catalog) { std::printf("FAIL (MakeCatalog: %s)\n", err.c_str()); return cleanup(1); }
  iceberg::TableIdentifier ident{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = table};
  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    std::printf("FAIL (LoadTable: %s)\n", loaded.error().message.c_str());
    return cleanup(1);
  }
  auto tbl = loaded.value();
  std::printf("OK\n");

  // --- (1) native read with [p, _pos] projection -----------------------------
  std::printf("[3/5] native read [p,_pos] ... ");
  std::vector<LiveRow> rows;
  if (!ReadLiveRows(StripFileScheme(tbl->metadata_file_location()), &rows, &err)) {
    std::printf("FAIL (%s)\n", err.c_str());
    return cleanup(1);
  }
  std::printf("%zu rows\n", rows.size());
  if (static_cast<int>(rows.size()) != kRows) {
    std::printf("      expected %d rows; aborting\n", kRows);
    return cleanup(1);
  }
  // Positions must be the absolute ordinals 0..kRows-1 (one data file).
  {
    std::set<int64_t> got;
    for (const auto& r : rows) {
      got.insert(r.pos);
      std::printf("        p=%-3lld _pos=%-2lld  file=%s\n",
                  static_cast<long long>(r.p), static_cast<long long>(r.pos),
                  fs::path(r.file).filename().c_str());
    }
    bool ok = got.size() == static_cast<size_t>(kRows) && *got.begin() == 0 &&
              *got.rbegin() == kRows - 1;
    if (!ok) {
      std::printf("      _pos set is not {0..%d}; projection wrong\n", kRows - 1);
      return cleanup(1);
    }
  }

  // --- (2) delete 2 rows by derived (file,pos), commit, native re-read -------
  std::printf("[4/5] delete 2 rows (p=7, p=23) via derived positions ... ");
  std::vector<LiveRow> targets;
  for (const auto& r : rows) if (r.p == 7 || r.p == 23) targets.push_back(r);
  if (targets.size() != 2) { std::printf("FAIL (targets)\n"); return cleanup(1); }
  const std::string del0 =
      StripFileScheme(tbl->location()) + "/data/morverify-del-0-" + name + ".parquet";
  double ms0 = DeleteAndCommit(tbl, del0, targets, &err);
  if (ms0 < 0) { std::printf("FAIL (%s)\n", err.c_str()); return cleanup(1); }
  auto reloaded = catalog->LoadTable(ident);
  if (!reloaded.has_value()) { std::printf("FAIL (reload)\n"); return cleanup(1); }
  tbl = reloaded.value();
  if (!ReadLiveRows(StripFileScheme(tbl->metadata_file_location()), &rows, &err)) {
    std::printf("FAIL (re-read: %s)\n", err.c_str());
    return cleanup(1);
  }
  bool gone = std::none_of(rows.begin(), rows.end(),
                           [](const LiveRow& r) { return r.p == 7 || r.p == 23; });
  std::printf("%zu rows (commit %.1f ms)\n", rows.size(), ms0);
  if (static_cast<int>(rows.size()) != kRows - 2 || !gone) {
    std::printf("      expected %d rows with p=7,p=23 absent; FAIL\n", kRows - 2);
    return cleanup(1);
  }

  // --- (3) commit cadence: 6 more single-row deletes back-to-back ------------
  std::printf("[5/5] cadence: 6 rapid single-row deletes ...\n");
  double total = ms0;
  int commits = 1;
  for (int k = 0; k < 6 && !rows.empty(); ++k) {
    LiveRow target = rows.front();  // delete the current first hole
    const std::string del =
        StripFileScheme(tbl->location()) + "/data/morverify-del-" +
        std::to_string(k + 1) + "-" + name + ".parquet";
    double ms = DeleteAndCommit(tbl, del, {target}, &err);
    if (ms < 0) { std::printf("      commit %d FAIL (%s)\n", k + 1, err.c_str()); return cleanup(1); }
    total += ms; commits++;
    auto rl = catalog->LoadTable(ident);
    if (!rl.has_value()) { std::printf("      reload FAIL\n"); return cleanup(1); }
    tbl = rl.value();
    if (!ReadLiveRows(StripFileScheme(tbl->metadata_file_location()), &rows, &err)) {
      std::printf("      re-read FAIL (%s)\n", err.c_str()); return cleanup(1);
    }
    std::printf("        commit %d: deleted p=%-3lld  %.1f ms  -> %zu live\n",
                k + 1, static_cast<long long>(target.p), ms, rows.size());
  }
  const int expected_final = kRows - 2 - 6;
  std::printf("\n  %d commits, avg %.1f ms/commit, final live=%zu (expected %d)\n",
              commits, total / commits, rows.size(), expected_final);
  bool ok = static_cast<int>(rows.size()) == expected_final;
  return cleanup(ok ? 0 : 1);
}

}  // namespace primeparts::catalog
