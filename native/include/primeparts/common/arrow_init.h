// primeparts/common/arrow_init.h
//
// One-time registration of the iceberg-cpp arrow IO + format factories
// (arrow filesystem, avro manifests, parquet data/delete). Scanning and
// manifest read/write need all three: an arrow-only registration fails
// PlanFiles with "Missing reader factory for file format: avro". Header-only;
// every entry point that opens Iceberg I/O calls EnsureArrowRegistration()
// instead of rolling its own RegisterAll sequence.

#pragma once

#include <mutex>

#include "iceberg/arrow/arrow_register.h"
#include "iceberg/avro/avro_register.h"
#include "iceberg/parquet/parquet_register.h"

namespace primeparts::common {

// Idempotent: registers arrow/avro/parquet exactly once per process,
// thread-safely. Safe to call from any entry point before opening a catalog or
// scanning a table.
inline void EnsureArrowRegistration() {
  static std::once_flag once;
  std::call_once(once, [] {
    iceberg::arrow::RegisterAll();
    iceberg::avro::RegisterAll();
    iceberg::parquet::RegisterAll();
  });
}

}  // namespace primeparts::common
