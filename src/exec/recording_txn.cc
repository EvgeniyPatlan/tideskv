#include "exec/recording_txn.h"

namespace tideskv {

Status RecordingTxn::DeletePrefix(ColumnFamily cf, std::string_view prefix) {
  Status st = inner_.DeletePrefix(cf, prefix);
  if (st.ok()) deletes_.push_back({cf, std::string(prefix), {}, true});
  return st;
}

Status RecordingTxn::DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) {
  Status st = inner_.DeleteRange(cf, lo, hi);
  if (st.ok()) deletes_.push_back({cf, std::string(lo), std::string(hi), false});
  return st;
}

Status RecordingTxn::Savepoint(std::string_view name) {
  Status st = inner_.Savepoint(name);
  if (st.ok()) savepoints_.emplace_back(std::string(name), deletes_.size());
  return st;
}

Status RecordingTxn::RollbackToSavepoint(std::string_view name) {
  Status st = inner_.RollbackToSavepoint(name);
  if (!st.ok()) return st;
  for (size_t i = savepoints_.size(); i-- > 0;) {
    if (savepoints_[i].first != name) continue;
    deletes_.resize(savepoints_[i].second);
    savepoints_.resize(i + 1);
    break;
  }
  return st;
}

Status RecordingTxn::Rollback() {
  deletes_.clear();
  savepoints_.clear();
  return inner_.Rollback();
}

}  // namespace tideskv
