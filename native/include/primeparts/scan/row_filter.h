#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "primeparts/scan/column_binder.h"

namespace arrow {

class RecordBatch;
}

namespace iceberg {

class Expression;
class Schema;
}

namespace primeparts::scan {

struct Interval {
  int64_t lo = std::numeric_limits<int64_t>::min();
  int64_t hi = std::numeric_limits<int64_t>::max();

  bool Contains(int64_t v) const { return v >= lo && v <= hi; }
  bool Empty() const { return lo > hi; }
  bool BoundedBelow() const { return lo != std::numeric_limits<int64_t>::min(); }
  bool BoundedAbove() const { return hi != std::numeric_limits<int64_t>::max(); }
};

struct FieldInterval {
  std::string field;
  Interval range;
};

class RowFilter {
 public:
  void Require(std::string field, Interval range);

  const std::vector<FieldInterval>& fields() const { return fields_; }

  const Interval* Find(std::string_view field) const;
  bool empty() const { return fields_.empty(); }

  bool Unsatisfiable() const;

  bool Pushdown(const iceberg::Schema& schema,
                std::shared_ptr<iceberg::Expression>* out,
                std::string* error) const;

 private:
  std::vector<FieldInterval> fields_;
};

class BoundRowFilter {
 public:
  bool Bind(const RowFilter& filter, const arrow::RecordBatch& batch,
            std::string* error);
  bool Test(int64_t row) const;

 private:
  std::vector<std::pair<WidenedColumn, Interval>> checks_;
};

class ValueRowFilter {
 public:
  bool Bind(const RowFilter& filter, const std::vector<std::string>& names,
            std::string* error);
  bool Test(const int64_t* values) const;

 private:
  std::vector<std::pair<std::size_t, Interval>> checks_;
};

}  // namespace primeparts::scan
