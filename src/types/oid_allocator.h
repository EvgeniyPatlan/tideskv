#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/status.h"
#include "encoding/replog_codec.h"
#include "storage/storage.h"

namespace tideskv {

// Hands out collection object ids (spec §4.3, M3 plan E1). Ids are never reused: blocks of
// kLeaseSize are leased by writing sys/oid_hwm in the allocator's own transaction (under its
// mutex, so it is the one shared key of invariant I1) and made durable through `persist`
// before any id of the block is returned. A restart resumes at the stored hwm, skipping the
// unused tail of the last block. Thread-safe.
class OidAllocator {
 public:
  static constexpr uint64_t kLeaseSize = 65536;
  static constexpr std::string_view kHwmKey = "oid_hwm";

  using CommitFn = std::function<Status(const std::vector<encoding::ReplOp>&)>;

  // persist makes the committed hwm durable (App: TidesStorage::Checkpoint); empty = no-op.
  // commit, when set, commits the lease's ops instead of the allocator's own transaction (App:
  // ReplLog::CommitStandalone, so the lease is replicated as its own batch).
  explicit OidAllocator(Storage& storage, std::function<Status()> persist = {}, CommitFn commit = {})
      : storage_(storage), persist_(std::move(persist)), commit_(std::move(commit)) {}

  // Reads sys/oid_hwm; a missing record starts at 1 (oid 0 is the strings' expire-index value).
  Status Init();
  // Internal error before Init(). A failed lease write is returned and retried by the next call.
  Result<uint64_t> Next();
  uint64_t hwm() const;  // exclusive end of the current lease

 private:
  Status Lease();                             // requires mu_
  Status WriteHwm(const std::string& value);  // requires mu_

  Storage& storage_;
  std::function<Status()> persist_;
  CommitFn commit_;
  mutable std::mutex mu_;
  bool initialized_ = false;
  uint64_t next_ = 0;   // next id to hand out
  uint64_t limit_ = 0;  // [next_, limit_) is leased and durable
};

}  // namespace tideskv
