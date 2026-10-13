#include "exec/recording_txn.h"

#include "encoding/key_codec.h"

namespace tideskv {

void RecordingTxn::Wrote(ColumnFamily cf, std::string_view key) {
  ++writes_;
  encoding::MetaKey mk;
  if (cf == ColumnFamily::kMeta && encoding::DecodeMetaKey(key, &mk)) {
    meta_keys_.push_back({mk.db, std::string(mk.user_key)});
  }
}

Status RecordingTxn::Put(ColumnFamily cf, std::string_view key, std::string_view value) {
  Status st = inner_.Put(cf, key, value);
  if (st.ok()) Wrote(cf, key);
  return st;
}

Status RecordingTxn::Delete(ColumnFamily cf, std::string_view key) {
  Status st = inner_.Delete(cf, key);
  if (st.ok()) Wrote(cf, key);
  return st;
}

Status RecordingTxn::DeletePrefix(ColumnFamily cf, std::string_view prefix) {
  Status st = inner_.DeletePrefix(cf, prefix);
  if (!st.ok()) return st;
  ++writes_;
  deletes_.push_back({cf, std::string(prefix), {}, true});
  return st;
}

Status RecordingTxn::DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) {
  Status st = inner_.DeleteRange(cf, lo, hi);
  if (!st.ok()) return st;
  ++writes_;
  deletes_.push_back({cf, std::string(lo), std::string(hi), false});
  return st;
}

Status RecordingTxn::Savepoint(std::string_view name) {
  Status st = inner_.Savepoint(name);
  if (st.ok()) savepoints_.push_back({std::string(name), deletes_.size(), writes_, meta_keys_.size()});
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
    savepoints_.resize(i + 1);
    break;
  }
  return st;
}

Status RecordingTxn::Rollback() {
  deletes_.clear();
  writes_ = 0;
  meta_keys_.clear();
  savepoints_.clear();
  return inner_.Rollback();
}

}  // namespace tideskv
