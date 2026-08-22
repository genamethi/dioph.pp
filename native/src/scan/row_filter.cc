#include "primeparts/scan/row_filter.h"

#include <algorithm>

#include <arrow/record_batch.h>

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/type.h"

namespace primeparts::scan {

namespace {

const iceberg::SchemaField* FieldByName(const iceberg::Schema& schema,
                                        const std::string& name) {
  for (const auto& f : schema.fields()) {
    if (f.name() == name) return &f;
  }
  return nullptr;
}

}  // namespace

void RowFilter::Require(std::string field, Interval range) {
  for (FieldInterval& f : fields_) {
    if (f.field == field) {
      f.range.lo = std::max(f.range.lo, range.lo);
      f.range.hi = std::min(f.range.hi, range.hi);
      return;
    }
  }
  fields_.push_back(FieldInterval{.field = std::move(field), .range = range});
}

const Interval* RowFilter::Find(std::string_view field) const {
  for (const FieldInterval& f : fields_) {
    if (f.field == field) return &f.range;
  }
  return nullptr;
}

bool RowFilter::Unsatisfiable() const {
  for (const FieldInterval& f : fields_) {
    if (f.range.Empty()) return true;
  }
  return false;
}

bool RowFilter::Pushdown(const iceberg::Schema& schema,
                         std::shared_ptr<iceberg::Expression>* out,
                         std::string* error) const {
  *out = nullptr;
  std::vector<std::shared_ptr<iceberg::Expression>> conjuncts;
  for (const FieldInterval& f : fields_) {
    const iceberg::SchemaField* field = FieldByName(schema, f.field);
    if (!field) {
      if (error) *error = "no column named '" + f.field + "' in this table";
      return false;
    }
    const iceberg::TypeId type = field->type()->type_id();
    if (type != iceberg::TypeId::kInt && type != iceberg::TypeId::kLong) {
      if (error) {
        *error = "column '" + f.field + "' is not an integer column: " +
                 field->type()->ToString();
      }
      return false;
    }
    const bool narrow = type == iceberg::TypeId::kInt;
    const bool lo_fits =
        !narrow || (f.range.lo >= std::numeric_limits<int32_t>::min() &&
                    f.range.lo <= std::numeric_limits<int32_t>::max());
    const bool hi_fits =
        !narrow || (f.range.hi >= std::numeric_limits<int32_t>::min() &&
                    f.range.hi <= std::numeric_limits<int32_t>::max());
    if (f.range.BoundedBelow() && lo_fits) {
      conjuncts.push_back(iceberg::Expressions::GreaterThanOrEqual(
          f.field, narrow ? iceberg::Literal::Int(static_cast<int32_t>(f.range.lo))
                          : iceberg::Literal::Long(f.range.lo)));
    }
    if (f.range.BoundedAbove() && hi_fits) {
      conjuncts.push_back(iceberg::Expressions::LessThanOrEqual(
          f.field, narrow ? iceberg::Literal::Int(static_cast<int32_t>(f.range.hi))
                          : iceberg::Literal::Long(f.range.hi)));
    }
  }
  if (conjuncts.empty()) return true;
  std::shared_ptr<iceberg::Expression> expr = conjuncts.front();
  for (std::size_t i = 1; i < conjuncts.size(); ++i) {
    expr = iceberg::Expressions::And(expr, conjuncts[i]);
  }
  *out = std::move(expr);
  return true;
}

bool BoundRowFilter::Bind(const RowFilter& filter,
                          const arrow::RecordBatch& batch, std::string* error) {
  checks_.clear();
  checks_.reserve(filter.fields().size());
  for (const FieldInterval& f : filter.fields()) {
    WidenedColumn col;
    if (!WidenedColumn::Bind(batch, f.field, &col, error)) return false;
    checks_.emplace_back(col, f.range);
  }
  return true;
}

bool BoundRowFilter::Test(int64_t row) const {
  for (const auto& [col, range] : checks_) {
    if (!range.Contains(col.Value(row))) return false;
  }
  return true;
}

bool ValueRowFilter::Bind(const RowFilter& filter,
                          const std::vector<std::string>& names,
                          std::string* error) {
  checks_.clear();
  checks_.reserve(filter.fields().size());
  for (const FieldInterval& f : filter.fields()) {
    const auto it = std::find(names.begin(), names.end(), f.field);
    if (it == names.end()) {
      if (error) {
        *error = "no value named '" + f.field + "' is produced here";
      }
      return false;
    }
    checks_.emplace_back(static_cast<std::size_t>(it - names.begin()), f.range);
  }
  return true;
}

bool ValueRowFilter::Test(const int64_t* values) const {
  for (const auto& [index, range] : checks_) {
    if (!range.Contains(values[index])) return false;
  }
  return true;
}

}  // namespace primeparts::scan
