#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "encoding/replog_codec.h"
#include "storage/storage.h"
#include "types/oid_allocator.h"

// Full sync as a logical snapshot stream (M6b D3, D4): the master walks a storage snapshot and sends its
// keyspace as PUT-only replog-v1 batches ("chunks"); the replica decodes and loads them.
namespace tideskv::repl {

inline constexpr size_t kLoadChunkBytes = size_t{4} << 20;  // D4
inline constexpr size_t kLoadChunkOps = 65536;

// *2 LOAD <chunk>, and *4 LOADEND <seq> <record> <ops> (D4).
std::string LoadFrame(std::string_view chunk);
std::string LoadEndFrame(uint64_t seq, std::string_view record, uint64_t ops);

// Whether an op may come from a master's snapshot (D3): a PUT into meta/subkey/zscore/expire, or sys/oid_hwm.
bool IsLoadableOp(const encoding::ReplOp& op);

// Decodes one LOAD chunk (a replog-v1 batch: CRC, structure) and checks every op is loadable.
Result<std::vector<encoding::ReplOp>> DecodeLoadChunk(std::string_view chunk);

// What a SnapshotChunker walks (M6b D3; M7b D2). The defaults are the full sync's: every key of meta, subkey,
// zscore and expire, then sys/oid_hwm. The slot migrator walks meta/subkey/zscore of one range, no sys.
struct ChunkerOptions {
  std::vector<ColumnFamily> cfs = {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore,
                                   ColumnFamily::kExpire};
  IterOptions bounds;  // applied to every CF's iterator; empty = the whole CF
  std::vector<std::string> sys_keys = {std::string(OidAllocator::kHwmKey)};  // PUT if present, in order
  size_t max_bytes = kLoadChunkBytes;
  size_t max_ops = kLoadChunkOps;
};

// Walks a snapshot view as PUT chunks of at most max_bytes / max_ops (one larger op is a chunk of its own).
// The iterator it holds must not outlive `view`.
class SnapshotChunker {
 public:
  SnapshotChunker(ReadView& view, int64_t timestamp_ms, ChunkerOptions options = {})
      : view_(view), timestamp_ms_(timestamp_ms), options_(std::move(options)) {}
  // The next encoded chunk; nullopt once done. between_keys (may be null) runs before each key is added;
  // an error from it ends the walk.
  Result<std::optional<std::string>> Next(const std::function<Status()>& between_keys);
  uint64_t ops() const { return ops_; }                           // ops in the chunks returned so far
  void SetMaxBytes(size_t bytes) { options_.max_bytes = bytes; }  // from the next chunk on

 private:
  Status NextOp(std::optional<encoding::ReplOp>* out);
  Status NextKeyspaceOp(std::optional<encoding::ReplOp>* out);
  Status NextSysOp(std::optional<encoding::ReplOp>* out);
  ReadView& view_;
  int64_t timestamp_ms_;
  ChunkerOptions options_;
  size_t cf_index_ = 0;
  std::unique_ptr<Iterator> it_;
  size_t sys_index_ = 0;
  std::optional<encoding::ReplOp> pending_;  // the op that did not fit the previous chunk
  uint64_t ops_ = 0;
};

}  // namespace tideskv::repl
