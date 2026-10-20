#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "storage/memory_storage.h"

namespace tideskv::test {

// InMemoryStorage whose write transactions fail on demand (M5b plan H10). A counter n >= 1
// makes the n-th next call of that kind fail with IOError and then disarms; -1 never fails.
// The counters are shared by every transaction of this storage.
class FaultyStorage final : public Storage {
 public:
  std::atomic<int> fail_commit_in{-1};
  std::atomic<int> fail_savepoint_in{-1};
  std::atomic<int> fail_rollback_to_savepoint_in{-1};
  std::atomic<int> fail_release_savepoint_in{-1};
  std::atomic<int> fail_begin_read_in{-1};  // BeginRead (M7b: a transient sys/migration load error)
  // Successful Savepoint calls minus successful ReleaseSavepoint calls, over every transaction.
  std::atomic<int> unreleased_savepoints{0};
  // Write transactions alive now, the most ever alive at once, and successful commits.
  std::atomic<int> open_txns{0};
  std::atomic<int> max_open_txns{0};
  std::atomic<int> commits{0};
  std::function<void()> before_commit;  // optional: runs at the start of every Commit (set it before use)

  Result<std::unique_ptr<WriteTxn>> BeginWrite() override {
    auto txn = inner_.BeginWrite();
    if (!txn.ok()) return txn.status();
    return std::unique_ptr<WriteTxn>(std::make_unique<Txn>(std::move(txn).value(), *this));
  }
  Result<std::unique_ptr<ReadView>> BeginRead() override {
    if (Trip(fail_begin_read_in)) return Status::IOError("injected read failure");
    return inner_.BeginRead();
  }
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override { return inner_.BeginSnapshot(); }
  std::shared_ptr<const InMemoryStorage::Tables> Current() const {
    return inner_.Current();
  }  // CommandHarness compares snapshots

  // True when this call is the armed one (and disarms the counter).
  static bool Trip(std::atomic<int>& counter) {
    int v = counter.load();
    while (v > 0) {
      if (counter.compare_exchange_weak(v, v == 1 ? -1 : v - 1)) return v == 1;
    }
    return false;
  }

 private:
  class Txn final : public WriteTxn {
   public:
    Txn(std::unique_ptr<WriteTxn> inner, FaultyStorage& owner) : inner_(std::move(inner)), owner_(owner) {
      const int open = ++owner_.open_txns;
      int max = owner_.max_open_txns.load();
      while (open > max && !owner_.max_open_txns.compare_exchange_weak(max, open)) {
      }
    }
    ~Txn() override { --owner_.open_txns; }
    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;
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
    Status Savepoint(std::string_view n) override {
      if (Trip(owner_.fail_savepoint_in)) return Status::IOError("injected savepoint failure");
      Status st = inner_->Savepoint(n);
      if (st.ok()) ++owner_.unreleased_savepoints;
      return st;
    }
    Status RollbackToSavepoint(std::string_view n) override {
      if (Trip(owner_.fail_rollback_to_savepoint_in)) return Status::IOError("injected rollback failure");
      return inner_->RollbackToSavepoint(n);
    }
    Status ReleaseSavepoint(std::string_view n) override {
      if (Trip(owner_.fail_release_savepoint_in)) return Status::IOError("injected release failure");
      Status st = inner_->ReleaseSavepoint(n);
      if (st.ok()) --owner_.unreleased_savepoints;
      return st;
    }
    Status Commit() override {
      if (owner_.before_commit) owner_.before_commit();
      if (Trip(owner_.fail_commit_in)) return Status::IOError("injected commit failure");
      Status st = inner_->Commit();
      if (st.ok()) ++owner_.commits;
      return st;
    }
    Status Rollback() override { return inner_->Rollback(); }

   private:
    std::unique_ptr<WriteTxn> inner_;
    FaultyStorage& owner_;
  };

  InMemoryStorage inner_;
};

}  // namespace tideskv::test
