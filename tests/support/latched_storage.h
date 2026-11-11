#pragma once
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "storage/memory_storage.h"

namespace tideskv::test {

// InMemoryStorage whose commits can be parked. Commits are numbered 1.. in the order they reach Commit().
// Hold(n) before the n-th commit arrives makes it park until Release(n); Release(n, false) then fails it
// with IOError("latched failure") and commits nothing. Lets a test hold one commit in flight while others
// run past it.
class LatchedStorage final : public Storage {
 public:
  void Hold(int n) {
    std::lock_guard lock(mu_);
    latches_[n] = Latch{};
  }
  void Release(int n, bool succeed = true) {
    std::lock_guard lock(mu_);
    Latch& latch = latches_[n];
    latch.released = true;
    latch.succeed = succeed;
    cv_.notify_all();
  }
  // Blocks until the n-th commit has reached Commit() and parked on its latch.
  void WaitParked(int n) {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return ParkedLocked(n); });
  }
  // WaitParked with a timeout, so that a test fails instead of hanging; true once parked.
  bool WaitParkedFor(int n, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, timeout, [&] { return ParkedLocked(n); });
  }
  int reached() const {
    std::lock_guard lock(mu_);
    return reached_;
  }

  Result<std::unique_ptr<WriteTxn>> BeginWrite() override {
    auto txn = inner_.BeginWrite();
    if (!txn.ok()) return txn.status();
    return std::unique_ptr<WriteTxn>(std::make_unique<Txn>(std::move(txn).value(), *this));
  }
  Result<std::unique_ptr<ReadView>> BeginRead() override { return inner_.BeginRead(); }
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override { return inner_.BeginSnapshot(); }

 private:
  struct Latch {
    bool parked = false;
    bool released = false;
    bool succeed = true;
  };

  bool ParkedLocked(int n) const {
    auto it = latches_.find(n);
    return it != latches_.end() && it->second.parked;
  }

  // True when the commit may go ahead; false when it was released to fail.
  bool Arrive() {
    std::unique_lock lock(mu_);
    const int n = ++reached_;
    cv_.notify_all();
    auto it = latches_.find(n);
    if (it == latches_.end()) return true;
    it->second.parked = true;
    cv_.notify_all();
    cv_.wait(lock, [&] { return it->second.released; });
    return it->second.succeed;
  }

  class Txn final : public WriteTxn {
   public:
    Txn(std::unique_ptr<WriteTxn> inner, LatchedStorage& owner) : inner_(std::move(inner)), owner_(owner) {}
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
      if (!owner_.Arrive()) return Status::IOError("latched failure");
      return inner_->Commit();
    }
    Status Rollback() override { return inner_->Rollback(); }

   private:
    std::unique_ptr<WriteTxn> inner_;
    LatchedStorage& owner_;
  };

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<int, Latch> latches_;  // std::map: references stay valid while others are added
  int reached_ = 0;
  InMemoryStorage inner_;
};

}  // namespace tideskv::test
