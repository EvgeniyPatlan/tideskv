#include "storage/column_family.h"

namespace tideskv {

std::string_view ColumnFamilyName(ColumnFamily cf) {
  switch (cf) {
    case ColumnFamily::kMeta:
      return "meta";
    case ColumnFamily::kSubkey:
      return "subkey";
    case ColumnFamily::kZScore:
      return "zscore";
    case ColumnFamily::kExpire:
      return "expire";
    case ColumnFamily::kReplog:
      return "replog";
    case ColumnFamily::kSys:
      return "sys";
  }
  return "unknown";
}

}  // namespace tideskv
