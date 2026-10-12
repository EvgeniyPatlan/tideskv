#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "exec/commit_listener.h"
#include "storage/storage.h"

namespace tideskv {

// Forwards everything to `inner` and remembers the successful DeletePrefix/DeleteRange calls,
// honouring savepoint rollbacks, so Engine can report them after commit.
class RecordingTxn final : public WriteTxn {
 public:
  explicit RecordingTxn(WriteTxn& inner) : inner_(inner) {}

  Result<std::string> Get(ColumnFamily cf, std::string_view key) override { return inner_.Get(cf, key); }
  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts) override {
    return inner_.NewIterator(cf, std::move(opts));
  }
  Status Put(ColumnFamily cf, std::string_view key, std::string_view value) override {
    return inner_.Put(cf, key, value);
  }
  Status Delete(ColumnFamily cf, std::string_view key) override { return inner_.Delete(cf, key); }
  Status DeletePrefix(ColumnFamily cf, std::string_view prefix) override;
  Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override;
  Status Savepoint(std::string_view name) override;
  Status RollbackToSavepoint(std::string_view name) override;
  Status Commit() override { return inner_.Commit(); }
  Status Rollback() override;

  const std::vector<RangeDelete>& deletes() const { return deletes_; }

 private:
  WriteTxn& inner_;
  std::vector<RangeDelete> deletes_;
  std::vector<std::pair<std::string, size_t>> savepoints_;  // name, deletes_.size() at the savepoint
};

}  // namespace tideskv
