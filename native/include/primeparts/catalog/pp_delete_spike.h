// primeparts/catalog/pp_delete_spike.h
//
// Throwaway proof harness for the native position-delete primitive
// (markdown/data_eng/delete_primitive_spike.md). Drives:
//   write delete file (PositionDeleteWriter) -> commit (RowDelta) ->
//   reopen table -> read back -> assert the deleted rows are gone.
//
// Uses the verified beeline path (scripts/hive_register.sh via pp_hive_sync) to
// create + seed + drop the throwaway table, and the native iceberg-cpp path for
// the delete write/commit/read under test.

#pragma once

#include <string>

namespace primeparts::catalog {

struct DeleteSpikeOptions {
  std::string rest_uri;     // IRC, for RowDelta commit (RestCatalog)
  std::string warehouse;    // on-disk warehouse root (…/ib-staging)
  bool keep = false;        // leave the throwaway table behind
};

// Run the spike end to end. Returns 0 on success (delete applied, count
// dropped by the expected amount), non-zero otherwise. Progress + the
// PASS/FAIL verdict are printed to stdout.
int RunDeleteSpike(const DeleteSpikeOptions& opts);

// De-risk the sieve's load-bearing mechanisms (markdown plan: "De-risk core
// mechanisms"). On a throwaway v2 (p, prime_rank) table, proves WITHOUT beeline
// read-back:
//   1. SourceTableReader can project [p, _pos] and the positions are correct
//      absolute data-file ordinals (and survive merge-on-read);
//   2. native MOR read-back returns the reduced set after a RowDelta commit;
//   3. ~10 rapid back-to-back RowDelta IRC commits are not throttled (timed).
// Positions to delete are derived from the scan (not hardcoded), exactly as the
// sieve will. Returns 0 on success. Reuses DeleteSpikeOptions.
int RunMorVerify(const DeleteSpikeOptions& opts);

}  // namespace primeparts::catalog
