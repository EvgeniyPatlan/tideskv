#include "exec/recording_txn.h"

#include <cstddef>

#include "encoding/key_codec.h"

namespace tideskv {

namespace {
Status ReplogRefused() { return Status::InvalidArgument("replog is written only by ReplLog"); }
}  // namespace

void RecordingTxn::Capture(encoding::ReplOpType type, ColumnFamily cf, std::string_view key,
                           std::string_view value) {
  if (capture_) ops_.push_back({type, cf, std::string(key), std::string(value)});
}

void RecordingTxn::Wrote(ColumnFamily cf, std::string_view key) {
  ++writes_;
  encoding::MetaKey mk;
  if (cf == ColumnFamily::kMeta && encoding::DecodeMetaKey(key, &mk)) {
    meta_keys_.push_back({mk.db, std::string(mk.user_key)});
  }
}

Status RecordingTxn::Put(ColumnFamily cf, std::string_view key, std::string_view value) {
  if (cf == ColumnFamily::kReplog) return ReplogRefused();
  Status st = inner_.Put(cf, key, value);
  if (!st.ok()) return st;
  Wrote(cf, key);
  Capture(encoding::ReplOpType::kPut, cf, key, value);
  return st;
}

Status RecordingTxn::Delete(ColumnFamily cf, std::string_view key) {
  if (cf == ColumnFamily::kReplog) return ReplogRefused();
  Status st = inner_.Delete(cf, key);
  if (!st.ok()) return st;
  Wrote(cf, key);
  Capture(encoding::ReplOpType::kDelete, cf, key, {});
  return st;
}

Status RecordingTxn::DeletePrefix(ColumnFamily cf, std::string_view prefix) {
  if (cf == ColumnFamily::kReplog) return ReplogRefused();
  Status st = inner_.DeletePrefix(cf, prefix);
  if (!st.ok()) return st;
  ++writes_;
  Capture(encoding::ReplOpType::kDeletePrefix, cf, prefix, {});
  deletes_.push_back({cf, std::string(prefix), {}, true});
  return st;
}

Status RecordingTxn::DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) {
  if (cf == ColumnFamily::kReplog) return ReplogRefused();
  Status st = inner_.DeleteRange(cf, lo, hi);
  if (!st.ok()) return st;
  ++writes_;
  Capture(encoding::ReplOpType::kDeleteRange, cf, lo, hi);
  deletes_.push_back({cf, std::string(lo), std::string(hi), false});
  return st;
}

Status RecordingTxn::Savepoint(std::string_view name) {
  Status st = inner_.Savepoint(name);
  if (st.ok())
    savepoints_.push_back({std::string(name), deletes_.size(), writes_, meta_keys_.size(), ops_.size()});
  return st;
}

Status RecordingTxn::RollbackToSavepoint(std::string_view name) {
  Status st = inner_.RollbackToSavepoint(name);
  if (!st.ok()) return st;
  for (size_t i = savepoints_.size(); i-- > 0;) {
    if (savepoints_[i].name != name) continue;
    deletes_.resize(savepoints_[i].deletes);
    writes_ = savepoints_[i].writes;
    meta_keys_.resize(savepoints_[i].meta_keys);
    ops_.resize(savepoints_[i].ops);
    savepoints_.resize(i + 1);
    break;
  }
  return st;
}

Status RecordingTxn::ReleaseSavepoint(std::string_view name) {
  Status st = inner_.ReleaseSavepoint(name);
  if (!st.ok()) return st;
  for (size_t i = savepoints_.size(); i-- > 0;) {  // newest first: in a batch it is the last one
    if (savepoints_[i].name != name) continue;
    savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(i));
    break;
  }
  return st;
}

Status RecordingTxn::Rollback() {
  deletes_.clear();
  writes_ = 0;
  meta_keys_.clear();
  ops_.clear();
  savepoints_.clear();
  return inner_.Rollback();
}

}  // namespace tideskv
