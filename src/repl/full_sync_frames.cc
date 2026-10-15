#include "repl/full_sync_frames.h"

#include "repl/socket_io.h"
#include "types/oid_allocator.h"

namespace tideskv::repl {
namespace {

constexpr ColumnFamily kLoadableCfs[] = {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore,
                                         ColumnFamily::kExpire};
constexpr size_t kOpOverhead = 16;  // op/cf bytes and the two varints, an upper bound

}  // namespace

std::string LoadFrame(std::string_view chunk) { return RespCommand({"LOAD", chunk}); }

std::string LoadEndFrame(uint64_t seq, std::string_view record, uint64_t ops) {
  const std::string s = std::to_string(seq);
  const std::string n = std::to_string(ops);
  return RespCommand({"LOADEND", s, record, n});
}

bool IsLoadableOp(const encoding::ReplOp& op) {
  if (op.type != encoding::ReplOpType::kPut) return false;
  if (op.cf == ColumnFamily::kSys) return op.key == OidAllocator::kHwmKey;
  for (ColumnFamily cf : kLoadableCfs) {
    if (op.cf == cf) return true;
  }
  return false;
}

Result<std::vector<encoding::ReplOp>> DecodeLoadChunk(std::string_view chunk) {
  auto batch = encoding::DecodeReplogBatch(chunk);
  if (!batch.ok()) return batch.status();
  for (const encoding::ReplOp& op : batch->ops) {
    if (!IsLoadableOp(op)) return Status::Corruption("full-sync chunk: an op that is not a snapshot PUT");
  }
  return std::move(batch->ops);
}

Status SnapshotChunker::NextOp(std::optional<encoding::ReplOp>* out) {
  out->reset();
  if (pending_) {
    out->swap(pending_);
    return Status::Ok();
  }
  while (cf_index_ < std::size(kLoadableCfs)) {
    const ColumnFamily cf = kLoadableCfs[cf_index_];
    Status st;
    if (!it_) {
      auto it = view_.NewIterator(cf);
      if (!it.ok()) return it.status();
      it_ = std::move(it).value();
      st = it_->SeekToFirst();
    } else {
      st = it_->Next();
    }
    if (!st.ok() && !st.IsNotFound()) return st;
    if (st.ok() && it_->Valid()) {
      *out = encoding::ReplOp{encoding::ReplOpType::kPut, cf, std::string(it_->key()),
                              std::string(it_->value())};
      return Status::Ok();
    }
    it_.reset();
    ++cf_index_;
  }
  if (sys_done_) return Status::Ok();
  sys_done_ = true;
  auto hwm = view_.Get(ColumnFamily::kSys, OidAllocator::kHwmKey);
  if (hwm.ok()) {
    *out = encoding::ReplOp{encoding::ReplOpType::kPut, ColumnFamily::kSys,
                            std::string(OidAllocator::kHwmKey), *hwm};
    return Status::Ok();
  }
  return hwm.status().IsNotFound() ? Status::Ok() : hwm.status();
}

Result<std::optional<std::string>> SnapshotChunker::Next(const std::function<Status()>& between_keys) {
  std::vector<encoding::ReplOp> ops;
  size_t bytes = 0;
  for (;;) {
    std::optional<encoding::ReplOp> op;
    if (Status st = NextOp(&op); !st.ok()) return st;
    if (!op) break;
    const size_t size = op->key.size() + op->value.size() + kOpOverhead;
    if (!ops.empty() && (bytes + size > kLoadChunkBytes || ops.size() >= kLoadChunkOps)) {
      pending_ = std::move(op);
      break;
    }
    if (between_keys) {
      if (Status st = between_keys(); !st.ok()) return st;
    }
    bytes += size;
    ops.push_back(std::move(*op));
  }
  if (ops.empty()) return std::optional<std::string>();
  ops_ += ops.size();
  return std::optional<std::string>(encoding::EncodeReplogBatch(ops, timestamp_ms_, 0));
}

}  // namespace tideskv::repl
