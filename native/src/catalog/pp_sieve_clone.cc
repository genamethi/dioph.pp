// primeparts/catalog/pp_sieve_clone.cc — see header.

#include "primeparts/catalog/pp_sieve_clone.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "primeparts/catalog/pp_iceberg_rest.h"

#include "iceberg/catalog.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_scan.h"
#include "iceberg/update/fast_append.h"

namespace primeparts::catalog {

namespace {

std::string StripFileScheme(std::string_view uri) {
  constexpr std::string_view kPfx = "file:";
  if (uri.substr(0, kPfx.size()) == kPfx) uri.remove_prefix(kPfx.size());
  return std::string(uri);
}

// Pull the first integer value for "key" out of a metadata.json blob. Handles
// both bare-number (`"format-version":2`) and quoted-number (`"total-records":
// "123"`) forms. Returns -1 if absent/unparseable.
long long FindJsonInt(const std::string& json, const std::string& key) {
  auto pos = json.find("\"" + key + "\"");
  if (pos == std::string::npos) return -1;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return -1;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '"')) ++pos;
  long long v = 0;
  bool any = false;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    v = v * 10 + (json[pos] - '0');
    ++pos;
    any = true;
  }
  return any ? v : -1;
}

// Scan a table and return (file count, summed record count). false on error.
bool ScanCounts(const std::shared_ptr<iceberg::Table>& tbl, int64_t* out_files,
                int64_t* out_records, std::string* error) {
  auto scan_b = tbl->NewScan();
  if (!scan_b.has_value()) { *error = "NewScan: " + scan_b.error().message; return false; }
  auto scan = scan_b.value()->Build();
  if (!scan.has_value()) { *error = "Build: " + scan.error().message; return false; }
  auto tasks = scan.value()->PlanFiles();
  if (!tasks.has_value()) { *error = "PlanFiles: " + tasks.error().message; return false; }
  int64_t files = 0, records = 0;
  for (const auto& t : tasks.value()) {
    ++files;
    records += t->data_file()->record_count;
  }
  *out_files = files;
  *out_records = records;
  return true;
}

}  // namespace

int RunCloneSieve(const CloneSieveOptions& opts) {
  std::printf("== pp-catalog clone-sieve ==\n");
  std::printf("  source   : primeparts.%s\n", opts.source_table.c_str());
  std::printf("  dest     : primeparts.%s\n", opts.dest_table.c_str());
  std::printf("  warehouse: %s\n", opts.warehouse.c_str());

  std::string err;
  auto catalog = MakeLocalCatalog(opts.warehouse, &err);
  if (!catalog) { std::printf("FAIL (MakeLocalCatalog: %s)\n", err.c_str()); return 1; }
  if (!EnsureNamespace(catalog, iceberg::Namespace{{"primeparts"}}, &err)) {
    std::printf("FAIL (EnsureNamespace: %s)\n", err.c_str());
    return 1;
  }

  // --- Load source MV (read-only) ------------------------------------------
  iceberg::TableIdentifier src_id{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = opts.source_table};
  auto src_r = catalog->LoadTable(src_id);
  if (!src_r.has_value()) {
    std::printf("FAIL (LoadTable %s: %s)\n", opts.source_table.c_str(),
                src_r.error().message.c_str());
    return 1;
  }
  auto src = src_r.value();
  auto schema_r = src->schema();
  auto spec_r = src->spec();
  if (!schema_r.has_value() || !spec_r.has_value()) {
    std::printf("FAIL (source schema/spec)\n");
    return 1;
  }

  // --- Collect the source's data files (no row copy) -----------------------
  int64_t src_files = 0, src_records = 0;
  {
    auto scan_b = src->NewScan();
    if (!scan_b.has_value()) { std::printf("FAIL (src NewScan: %s)\n", scan_b.error().message.c_str()); return 1; }
    auto scan = scan_b.value()->Build();
    if (!scan.has_value()) { std::printf("FAIL (src Build: %s)\n", scan.error().message.c_str()); return 1; }
    auto tasks = scan.value()->PlanFiles();
    if (!tasks.has_value()) { std::printf("FAIL (src PlanFiles: %s)\n", tasks.error().message.c_str()); return 1; }

    std::vector<std::shared_ptr<iceberg::DataFile>> files;
    for (const auto& t : tasks.value()) {
      auto df = t->data_file();
      src_records += df->record_count;
      ++src_files;
      files.push_back(df);
    }
    std::printf("  source has %lld data files, %lld records\n",
                static_cast<long long>(src_files),
                static_cast<long long>(src_records));
    if (files.empty()) { std::printf("FAIL (source has no data files)\n"); return 1; }

    // --- Drop any prior dest (purge=false: never delete the shared files) ---
    iceberg::TableIdentifier dst_id{
        .ns = iceberg::Namespace{{"primeparts"}}, .name = opts.dest_table};
    auto dropped = catalog->DropTable(dst_id, /*purge=*/false);
    (void)dropped;  // NotFound is fine — first run.

    // --- Create dest: v2, MOR, unpartitioned, same schema/spec --------------
    const std::string dst_location =
        opts.warehouse + "/primeparts.db/" + opts.dest_table;
    std::unordered_map<std::string, std::string> props = {
        {"format-version", "2"},
        {"write.delete.mode", "merge-on-read"},
        {"write.merge.mode", "merge-on-read"},
        {"write.update.mode", "merge-on-read"},
        {"write.parquet.compression-codec", "zstd"},
    };
    auto created =
        catalog->CreateTable(dst_id, schema_r.value(), spec_r.value(),
                             iceberg::SortOrder::Unsorted(), dst_location, props);
    if (!created.has_value()) {
      std::printf("FAIL (CreateTable: %s)\n", created.error().message.c_str());
      std::printf(
          "  -> this HMS REST servlet did not honor native CreateTable.\n"
          "     Fall back to the proven Hive-DDL shell, then re-run --clone-sieve\n"
          "     (it will DropTable the shell and CreateTable again, OR adapt to\n"
          "     FastAppend onto the existing shell):\n"
          "     pp-catalog --hive-exec \"CREATE TABLE primeparts.%s "
          "(p bigint, prime_rank bigint) STORED BY ICEBERG STORED AS PARQUET "
          "TBLPROPERTIES('format-version'='2','write.delete.mode'='merge-on-read',"
          "'write.merge.mode'='merge-on-read','write.update.mode'='merge-on-read')\"\n",
          opts.dest_table.c_str());
      return 1;
    }
    auto dst = created.value();

    // --- FastAppend the source's data files (the shallow clone) -------------
    auto app_r = dst->NewFastAppend();
    if (!app_r.has_value()) {
      std::printf("FAIL (NewFastAppend: %s)\n", app_r.error().message.c_str());
      return 1;
    }
    auto app = std::move(app_r.value());
    for (const auto& f : files) app->AppendFile(f);
    if (auto s = app->Commit(); !s.has_value()) {
      std::printf("FAIL (FastAppend Commit: %s)\n", s.error().message.c_str());
      return 1;
    }
    if (auto s = dst->Refresh(); !s.has_value()) {
      std::printf("FAIL (Refresh: %s)\n", s.error().message.c_str());
      return 1;
    }
    const std::string md_loc(dst->metadata_file_location());
    std::printf("  created + appended; metadata: %s\n", md_loc.c_str());

    // --- Verify: format-version from metadata.json; counts via re-scan ------
    long long fv = -1;
    {
      std::ifstream f(StripFileScheme(md_loc));
      if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        fv = FindJsonInt(ss.str(), "format-version");
      }
    }
    int64_t dst_files = 0, dst_records = 0;
    std::string scan_err;
    bool scanned = ScanCounts(dst, &dst_files, &dst_records, &scan_err);
    if (!scanned) std::printf("  WARN (verify re-scan: %s)\n", scan_err.c_str());

    std::printf("  verify: format-version=%lld  data-files=%lld  records=%lld\n",
                fv, static_cast<long long>(dst_files),
                static_cast<long long>(dst_records));

    bool ok = (fv == 2) && scanned && dst_files == src_files &&
              dst_records == src_records;
    if (fv != 2) {
      std::printf(
          "  NOTE: format-version is %lld, not 2 — position deletes require v2.\n"
          "        Drop this table and use the Hive-DDL shell fallback.\n",
          fv);
    }
    std::printf("\n== clone-sieve %s ==\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
  }
}

}  // namespace primeparts::catalog
