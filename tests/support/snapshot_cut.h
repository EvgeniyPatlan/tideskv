#pragma once
#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "storage/storage.h"

namespace tideskv::test {

// What "an exact cut" means at its boundary (M6b D2), checked cheaply: replog[seq] and every keyspace
// write of its batch are in the view, replog[seq+1] is not. Returns false on a mismatch.
inline bool CutBoundaryHolds(const SyncSnapshot& snap) {
  ReadView& v = *snap.view;
  if (!v.Get(ColumnFamily::kReplog, encoding::ReplogKey(snap.seq + 1)).status().IsNotFound()) return false;
  if (snap.seq == 0) return true;
  auto record = v.Get(ColumnFamily::kReplog, encoding::ReplogKey(snap.seq));
  if (!record.ok()) return false;
  auto batch = encoding::DecodeReplogBatch(*record);
  if (!batch.ok()) return false;
  for (const encoding::ReplOp& op : batch->ops) {
    if (!v.Get(op.cf, op.key).ok()) return false;
  }
  return true;
}

}  // namespace tideskv::test
