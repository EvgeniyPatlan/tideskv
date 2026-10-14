#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "storage/memory_storage.h"

namespace tideskv::test {

// Forwards to a real transaction but fails the first `failures` commits with Conflict.
class ConflictingTxn final : public WriteTxn {
 public:
  ConflictingTxn(std::unique_ptr<WriteTxn> inner, std::atomic<int>* failures)
      : inner_(std::move(inner)), failures_(failures) {}
  Result<std::string> Get(ColumnFamily cf, std::string_view k) override { return inner_->Get(cf, k); }
  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions o) override {
    return inner_->NewIterator(cf, std::move(o));
  }
  Status Put(ColumnFamily cf, std::string_view k, std::string_view v) override {
    return inner_->Put(cf, k, v);
  }
  Status Delete(ColumnFamily cf, std::string_view k) override { return inner_->Delete(cf, k); }
  Status DeletePrefix(ColumnFamily cf, std::string_view p) override { return inner_->DeletePrefix(cf, p); }
  Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
    return inner_->DeleteRange(cf, lo, hi);
  }
  Status Savepoint(std::string_view n) override { return inner_->Savepoint(n); }
  Status RollbackToSavepoint(std::string_view n) override { return inner_->RollbackToSavepoint(n); }
  Status ReleaseSavepoint(std::string_view n) override { return inner_->ReleaseSavepoint(n); }
  Status Commit() override {
    if (failures_->fetch_sub(1) > 0) return Status::Conflict("injected");
    return inner_->Commit();
  }
  Status Rollback() override { return inner_->Rollback(); }

 private:
  std::unique_ptr<WriteTxn> inner_;
  std::atomic<int>* failures_;
};

class ConflictingStorage final : public Storage {
 public:
  explicit ConflictingStorage(int failures) : failures_(failures) {}
  Result<std::unique_ptr<WriteTxn>> BeginWrite() override {
    auto txn = inner_.BeginWrite();
    if (!txn.ok()) return txn.status();
    return std::unique_ptr<WriteTxn>(std::make_unique<ConflictingTxn>(std::move(txn).value(), &failures_));
  }
  Result<std::unique_ptr<ReadView>> BeginRead() override { return inner_.BeginRead(); }
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override { return inner_.BeginSnapshot(); }

 private:
  InMemoryStorage inner_;
  std::atomic<int> failures_;
};

}  // namespace tideskv::test
