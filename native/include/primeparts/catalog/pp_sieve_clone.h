// primeparts/catalog/pp_sieve_clone.h
//
// One-shot: stand up the sieve's delete-target table primeparts.primes_k0_sieve
// as a metadata-only SHALLOW CLONE of primeparts.primes_k0.
//
// primes_k0 is already flat/unpartitioned, format-version 2, merge-on-read
// (111 data files, 3.87B rows). The sieve must delete from a SEPARATE table so
// the canonical MV is never mutated. Rather than copy 3.87B rows, we create a
// new v2 table whose manifest RE-REFERENCES primes_k0's existing data files (by
// path) via a native IRC CreateTable + FastAppend — zero row-copy. Our
// position-delete files then land only in primes_k0_sieve's metadata tree.
//
// CAVEAT: the two tables share physical data files, so do NOT `REBUILD` the
// primes_k0 source (or run orphan-file cleanup on it) while the sieve campaign
// is live, or the clone's referenced files could be replaced/removed.
//
// v2 verification: the REST CreateTableRequest carries no explicit
// format-version field, so this tool reads the resulting metadata.json back and
// asserts format-version==2.

#pragma once

#include <string>

namespace primeparts::catalog {

struct CloneSieveOptions {
  std::string warehouse;                       // on-disk warehouse root (…/ib-staging)
  std::string source_table = "primes_k0";      // table to clone (read-only)
  std::string dest_table = "primes_k0_sieve";  // delete target to create
};

// Create dest_table as a v2 MOR shallow clone of source_table and verify it.
// Returns 0 on success (v2, file count + record count match the source).
// Progress + PASS/FAIL printed to stdout.
int RunCloneSieve(const CloneSieveOptions& opts);

}  // namespace primeparts::catalog
