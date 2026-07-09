#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
}
namespace iceberg {
class Expression;
}

namespace primeparts::verify {

struct Window {
  int64_t p_lo = 0;
  int64_t p_hi = 0;
  int64_t limit = 0;
};

std::shared_ptr<iceberg::Expression> BuildWindowFilter(const Window& w);

struct Violation {
  int64_t p = 0;
  std::string detail;
};

struct CheckResult {
  std::string table;
  int64_t rows_checked = 0;
  int64_t violations = 0;
  std::vector<Violation> examples;
  bool ok() const { return violations == 0; }
};

struct CheckSpec {
  std::string table;
  std::vector<std::string> select;
};

class ShardState {
 public:
  virtual ~ShardState() = default;
};

class Check {
 public:
  virtual ~Check() = default;
  virtual const CheckSpec& spec() const = 0;
  virtual std::unique_ptr<ShardState> NewShard() const = 0;
  virtual void Eval(const arrow::RecordBatch& batch,
                    const std::string& data_file, ShardState& state,
                    CheckResult& out, int max_examples) const = 0;
};

std::unique_ptr<Check> MakePrimeRankCheck();
std::unique_ptr<Check> MakePartitionCheck();

class TableVerifier {
 public:
  static CheckResult Run(const std::string& metadata_path, const Check& check,
                         std::shared_ptr<iceberg::Expression> filter,
                         int threads, int64_t limit, int max_examples,
                         std::optional<int64_t> from_snapshot_exclusive,
                         std::string* error);
};

}
