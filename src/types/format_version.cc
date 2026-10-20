#include "types/format_version.h"

#include <optional>
#include <string>

#include "encoding/replog_codec.h"

namespace tideskv {
namespace {

using encoding::KeyLayout;

std::optional<KeyLayout> LayoutOf(std::string_view version) {
  if (version == kFormatVersionSlotted) return KeyLayout::kSlotted;
  if (version == kFormatVersionSlotless) return KeyLayout::kSlotless;
  return std::nullopt;
}

Result<bool> AnyEntry(WriteTxn& txn, ColumnFamily cf) {
  auto it = txn.NewIterator(cf);
  if (!it.ok()) return it.status();
  if (Status st = (*it)->SeekToFirst(); !st.ok() && !st.IsNotFound()) return st;
  return (*it)->Valid();
}

// No keyspace entry, and no logged op that a PSYNC CONTINUE could still ship (baseline records have none).
Result<bool> HoldsNothingInTheOldLayout(WriteTxn& txn) {
  for (ColumnFamily cf :
       {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore, ColumnFamily::kExpire}) {
    auto any = AnyEntry(txn, cf);
    if (!any.ok()) return any.status();
    if (*any) return false;
  }
  auto it = txn.NewIterator(ColumnFamily::kReplog);
  if (!it.ok()) return it.status();
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    auto batch = encoding::DecodeReplogBatch((*it)->value());
    if (!batch.ok()) return batch.status();
    if (!batch->ops.empty()) return false;
  }
  if (!st.ok() && !st.IsNotFound()) return st;
  return true;
}

Status Write(WriteTxn& txn, KeyLayout layout) {
  if (Status st = txn.Put(ColumnFamily::kSys, kFormatVersionKey, FormatVersionFor(layout)); !st.ok())
    return st;
  return txn.Commit();
}

}  // namespace

std::string_view FormatVersionFor(KeyLayout layout) {
  return layout == KeyLayout::kSlotless ? kFormatVersionSlotless : kFormatVersionSlotted;
}

Result<OpenedFormat> OpenFormat(Storage& storage, bool cluster_enabled) {
  const KeyLayout configured = cluster_enabled ? KeyLayout::kSlotted : KeyLayout::kSlotless;
  auto txn = storage.BeginWrite();
  if (!txn.ok()) return txn.status();
  auto current = (*txn)->Get(ColumnFamily::kSys, kFormatVersionKey);
  if (!current.ok() && !current.status().IsNotFound()) return current.status();
  if (!current.ok()) {
    if (Status st = Write(**txn, configured); !st.ok()) return st;
    return OpenedFormat{configured, FormatOutcome::kCreated};
  }
  const std::optional<KeyLayout> stored = LayoutOf(*current);
  if (!stored) {
    return Status::Corruption("unsupported data format version " + *current + " (this build reads versions " +
                              std::string(kFormatVersionSlotted) + " and " +
                              std::string(kFormatVersionSlotless) + ")");
  }
  if (*stored == configured) return OpenedFormat{configured, FormatOutcome::kKept};
  auto empty = HoldsNothingInTheOldLayout(**txn);
  if (!empty.ok()) return empty.status();
  if (*empty) {
    if (Status st = Write(**txn, configured); !st.ok()) return st;
    return OpenedFormat{configured, FormatOutcome::kSwitched};
  }
  if (cluster_enabled) {
    return Status::InvalidArgument(
        "cluster-enabled yes, but this data directory uses the standalone key layout (format 2); "
        "cluster mode needs format 1. Start with an empty dir, or convert it offline");
  }
  return OpenedFormat{KeyLayout::kSlotted, FormatOutcome::kLegacy};
}

}  // namespace tideskv
