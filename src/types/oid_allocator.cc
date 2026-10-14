#include "types/oid_allocator.h"

#include <string>

#include "common/coding.h"

namespace tideskv {

Status OidAllocator::Init() {
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  auto raw = (*view)->Get(ColumnFamily::kSys, kHwmKey);
  uint64_t hwm = 1;
  if (raw.ok()) {
    std::string_view in = *raw;
    if (in.size() != 8 || !coding::GetFixed64BE(&in, &hwm) || hwm == 0) {
      return Status::Corruption("sys/oid_hwm is not a valid 8-byte id");
    }
  } else if (!raw.status().IsNotFound()) {
    return raw.status();
  }
  std::lock_guard lock(mu_);
  next_ = limit_ = hwm;
  initialized_ = true;
  return Status::Ok();
}

Status OidAllocator::WriteHwm(const std::string& value) {
  if (commit_)
    return commit_({{encoding::ReplOpType::kPut, ColumnFamily::kSys, std::string(kHwmKey), value}});
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = (*txn)->Put(ColumnFamily::kSys, kHwmKey, value); !st.ok()) return st;
  return (*txn)->Commit();
}

Status OidAllocator::Lease() {
  const uint64_t new_limit = limit_ + kLeaseSize;
  std::string value;
  coding::PutFixed64BE(&value, new_limit);
  if (Status st = WriteHwm(value); !st.ok()) return st;
  if (persist_) {
    if (Status st = persist_(); !st.ok()) return st;
  }
  limit_ = new_limit;
  return Status::Ok();
}

Result<uint64_t> OidAllocator::Next() {
  std::lock_guard lock(mu_);
  if (!initialized_) return Status::Internal("OidAllocator::Next before Init");
  if (next_ == limit_) {
    if (Status st = Lease(); !st.ok()) return st;
  }
  return next_++;
}

uint64_t OidAllocator::hwm() const {
  std::lock_guard lock(mu_);
  return limit_;
}

}  // namespace tideskv
