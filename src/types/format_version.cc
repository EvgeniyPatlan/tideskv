#include "types/format_version.h"

#include <string>

namespace tideskv {

Status EnsureFormatVersion(Storage& storage) {
  auto txn = storage.BeginWrite();
  if (!txn.ok()) return txn.status();
  auto current = (*txn)->Get(ColumnFamily::kSys, kFormatVersionKey);
  if (current.ok()) {
    if (*current == kFormatVersion) return Status::Ok();
    return Status::Corruption("unsupported data format version " + *current + " (this build reads version " +
                              std::string(kFormatVersion) + ")");
  }
  if (!current.status().IsNotFound()) return current.status();
  if (Status st = (*txn)->Put(ColumnFamily::kSys, kFormatVersionKey, kFormatVersion); !st.ok()) return st;
  return (*txn)->Commit();
}

}  // namespace tideskv
