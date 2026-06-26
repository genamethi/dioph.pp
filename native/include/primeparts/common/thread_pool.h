// primeparts/common/thread_pool.h
//
// Size Arrow's CPU + IO thread pools in one call. Header-only. Used by the
// multithreaded scan/sift tools (covering-sieve, primitive-factors, …) that
// match Arrow's pool capacity to their shard/worker count.

#pragma once

#include <string>

#include <arrow/io/interfaces.h>
#include <arrow/util/thread_pool.h>

namespace primeparts::common {

// Set both the CPU and IO thread-pool capacities to `threads`. Returns true on
// success; on failure returns false and (if `error` non-null) sets it to a
// human-readable message. Callers that don't care about the outcome can ignore
// the result: `(void)SetupArrowThreadPools(n);`.
inline bool SetupArrowThreadPools(int threads, std::string* error = nullptr) {
  auto cpu = arrow::SetCpuThreadPoolCapacity(threads);
  if (!cpu.ok()) {
    if (error) *error = "set Arrow CPU pool: " + cpu.ToString();
    return false;
  }
  auto io = arrow::io::SetIOThreadPoolCapacity(threads);
  if (!io.ok()) {
    if (error) *error = "set Arrow IO pool: " + io.ToString();
    return false;
  }
  return true;
}

}  // namespace primeparts::common
