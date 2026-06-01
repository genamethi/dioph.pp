// primeparts/catalog/pp_hive_sync.h
//
// Hive-engine catalog sync for primeparts. Deliberately thin: there is no
// iceberg-cpp API for making the Hive engine adopt an externally-committed
// snapshot — the mechanism is `ALTER TABLE ... SET
// TBLPROPERTIES('metadata_location'=...)` issued over beeline, which is a
// subprocess to the JVM. So this wraps scripts/hive_register.sh rather than
// reimplementing a JDBC client in C++.
//
// Verified 2026-05-31: the beeline ALTER causes the HiveIcebergStorageHandler
// to load the target snapshot and re-commit it as a new metadata.json, so the
// engine sees the change. See HANDOFF.md "HMS sync is a separate, required
// step" and memory project-hms-sync-beeline-verified.

#pragma once

#include <string>
#include <vector>

namespace primeparts::catalog {

/// Path resolution + invocation settings for scripts/hive_register.sh.
struct HiveSyncOptions {
  // Absolute path to scripts/hive_register.sh. Empty => resolve relative to the
  // running binary's directory (../scripts/hive_register.sh) then $PATH.
  std::string script_path;
};

/// Run `hive_register.sh <subcommand> <args...>` as a subprocess. Captures
/// combined stdout+stderr into `*output`. Returns the script's exit code, or
/// -1 if the subprocess could not be launched (with the reason in `*output`).
int RunHiveRegister(const HiveSyncOptions& opts,
                    const std::vector<std::string>& args, std::string* output);

/// Convenience: `hive_register.sh sync <db_table> <metadata_uri>`. Returns true
/// on exit 0. `*output` holds combined output for logging.
bool HiveSync(const HiveSyncOptions& opts, const std::string& db_table,
              const std::string& metadata_uri, std::string* output);

/// Convenience: `hive_register.sh exec <sql>`.
bool HiveExec(const HiveSyncOptions& opts, const std::string& sql,
              std::string* output);

}  // namespace primeparts::catalog
