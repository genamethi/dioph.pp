#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "primeparts/query/query_preset.h"

namespace iceberg {
struct Namespace;
}

namespace primeparts::query {

namespace fs = std::filesystem;

struct PrimeInfo {
  int64_t p = 0;
  int32_t k = 0;
  int64_t prime_rank = 0;
};

struct PartitionTuple {
  int32_t m_k = 0;
  int32_t n_k = 0;
  int64_t q_k = 0;
};

struct ScanHit {
  int64_t p = 0;
  int64_t prime_rank = 0;
};

struct GroupCountRow {
  int64_t value = 0;
  int64_t count = 0;
};

struct GroupKey {
  std::string column;
  std::vector<std::string> inputs;
  std::function<int64_t(const int64_t* vals)> fn;

  static GroupKey Column(std::string name) {
    GroupKey k;
    k.column = std::move(name);
    return k;
  }
  static GroupKey Derived(std::vector<std::string> inputs,
                          std::function<int64_t(const int64_t* vals)> fn) {
    GroupKey k;
    k.inputs = std::move(inputs);
    k.fn = std::move(fn);
    return k;
  }
  bool derived() const { return fn != nullptr; }
};

struct TableRows {
  std::vector<std::string> cols;
  std::vector<std::vector<int64_t>> rows;
};

struct TableExtent {
  std::string table;
  bool ok = false;
  int64_t row_count = -1;
  int64_t data_files = -1;
  int64_t file_bytes = -1;
  int64_t snapshots = 0;
  int64_t snapshot_id = -1;
  int64_t sequence = -1;
  std::string key_name;
  int64_t key_max = -1;
};

struct ScanControl {
  std::atomic<bool>* cancel = nullptr;
  std::function<void(int64_t scanned, int64_t total)> progress;
};

class QueryService {
 public:
  static std::unique_ptr<QueryService> Open(const fs::path& warehouse,
                                            const std::string& rest_uri,
                                            const iceberg::Namespace& ns,
                                            std::string* error);
  ~QueryService();
  QueryService(const QueryService&) = delete;
  QueryService& operator=(const QueryService&) = delete;

  std::optional<PrimeInfo> LookupPrime(int64_t p, std::string* error,
                                       const ScanControl& ctl = {});

  std::vector<PartitionTuple> LookupPartitions(int64_t p, std::string* error,
                                               const ScanControl& ctl = {});

  std::vector<ScanHit> ScanByK(int32_t k, int64_t p_lo, int64_t p_hi,
                               int64_t limit, std::string* error,
                               const ScanControl& ctl = {});

  std::vector<GroupCountRow> GroupCount(const std::string& table,
                                        const GroupKey& key,
                                        int64_t p_lo, int64_t p_hi, int threads,
                                        std::string* error,
                                        const ScanControl& ctl = {});

  bool Materialize(const std::string& name,
                   const std::vector<std::string>& col_names,
                   const std::vector<std::vector<int64_t>>& columns,
                   std::string* metadata_location, std::string* error);

  TableRows ReadTable(const std::string& table,
                      const std::vector<std::string>& cols, int64_t limit,
                      std::string* error);

  const std::vector<std::string>& SchemaFields();

  bool ValidatePreset(const QueryPreset& p, std::string* error);

  std::vector<std::string> ListTables(std::string* error);

  TableExtent Extent(const std::string& table, bool with_key_max,
                     std::string* error, const ScanControl& ctl = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit QueryService(std::unique_ptr<Impl> impl);
};

}  // namespace primeparts::query
