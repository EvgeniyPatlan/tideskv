#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/commit_listener.h"
#include "exec/write_observer.h"
#include "storage/storage.h"

namespace tideskv {

// Forwards everything to `inner` and remembers the successful DeletePrefix/DeleteRange calls, the
// write count and the meta keys written, honouring savepoint rollbacks, so Engine can report them.
class RecordingTxn final : public WriteTxn {
 public:
  explicit RecordingTxn(WriteTxn& inner) : inner_(inner) {}

  Result<std::string> Get(ColumnFamily cf, std::string_view key) override { return inner_.Get(cf, key); }
  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts) override {
    return inner_.NewIterator(cf, std::move(opts));
  }
  Status Put(ColumnFamily cf, std::string_view key, std::string_view value) override;
  Status Delete(ColumnFamily cf, std::string_view key) override;
  Status DeletePrefix(ColumnFamily cf, std::string_view prefix) override;
  Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override;
  Status Savepoint(std::string_view name) override;
  Status RollbackToSavepoint(std::string_view name) override;
  Status ReleaseSavepoint(std::string_view name) override;  // keeps the counts; forgets the savepoint
  Status Commit() override { return inner_.Commit(); }
  Status Rollback() override;

  const std::vector<RangeDelete>& deletes() const { return deletes_; }
  // Successful mutations (Put, Delete, DeletePrefix, DeleteRange), minus those rolled back.
  size_t writes() const { return writes_; }
  // Keys whose meta record was put or deleted, in write order; may repeat (M5a plan G2).
  const std::vector<DbKey>& meta_keys() const { return meta_keys_; }
  // Replication capture (M6a D5): off by default. When on, every successful mutation is recorded as
  // one physical op, exactly as called, in program order and minus those rolled back.
  void CaptureOps(bool on) { capture_ = on; }
  const std::vector<encoding::ReplOp>& ops() const { return ops_; }

 private:
  void Wrote(ColumnFamily cf, std::string_view key);  // after a successful Put/Delete
  void Capture(encoding::ReplOpType type, ColumnFamily cf, std::string_view key, std::string_view value);
  struct Mark {
    std::string name;
    size_t deletes, writes, meta_keys, ops;  // sizes at the savepoint
  };

  WriteTxn& inner_;
  std::vector<RangeDelete> deletes_;
  size_t writes_ = 0;
  std::vector<DbKey> meta_keys_;
  bool capture_ = false;
  std::vector<encoding::ReplOp> ops_;
  std::vector<Mark> savepoints_;
};

}  // namespace tideskv
