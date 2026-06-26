// primeparts/common/uri.h
//
// Tiny URI helpers shared across modules. Header-only.

#pragma once

#include <string>
#include <string_view>

namespace primeparts::common {

// Strip a leading `file://` or `file:` scheme from an iceberg location, leaving
// an absolute filesystem path. Iceberg/Hadoop emit both forms (`file:///abs`
// and single-slash `file:/abs`); both map to `/abs`. A path with no scheme is
// returned unchanged.
inline std::string StripFileScheme(std::string_view uri) {
  if (uri.rfind("file://", 0) == 0) {
    uri.remove_prefix(7);
  } else if (uri.rfind("file:", 0) == 0) {
    uri.remove_prefix(5);
  }
  return std::string(uri);
}

}  // namespace primeparts::common
