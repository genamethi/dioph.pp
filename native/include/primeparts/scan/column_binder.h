#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <arrow/array.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>

namespace primeparts::scan {

inline const int64_t* BindInt64(const arrow::RecordBatch& batch,
                                std::string_view name, std::string* error) {
  auto col = batch.GetColumnByName(std::string(name));
  if (!col) {
    if (error) *error = "column not in batch: " + std::string(name);
    return nullptr;
  }
  if (col->type_id() != arrow::Type::INT64) {
    if (error) {
      *error = "column " + std::string(name) +
               " is not int64: " + col->type()->ToString();
    }
    return nullptr;
  }
  return static_cast<const arrow::Int64Array&>(*col).raw_values();
}

inline const int32_t* BindInt32(const arrow::RecordBatch& batch,
                                std::string_view name, std::string* error) {
  auto col = batch.GetColumnByName(std::string(name));
  if (!col) {
    if (error) *error = "column not in batch: " + std::string(name);
    return nullptr;
  }
  if (col->type_id() != arrow::Type::INT32) {
    if (error) {
      *error = "column " + std::string(name) +
               " is not int32: " + col->type()->ToString();
    }
    return nullptr;
  }
  return static_cast<const arrow::Int32Array&>(*col).raw_values();
}

}  // namespace primeparts::scan
