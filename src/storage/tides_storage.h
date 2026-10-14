#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "storage/compactor.h"
#include "storage/storage.h"

namespace tideskv {

enum class SyncMode : uint8_t { kNone, kInterval, kFull };

enum class Compression : uint8_t { kNone, kLz4, kZstd };
std::string_view CompressionName(Compression c);  // "no", "lz4", "zstd"

struct CfStats {
  uint64_t total_keys = 0;        // in sstables (not the memtable)
  uint64_t total_tombstones = 0;  // point + range tombstones in sstables
  uint64_t compaction_count = 0;
};

struct StorageStats {
  std::array<CfStats, kColumnFamilyCount> cf{};
  uint64_t flush_count = 0;
  uint64_t compaction_count = 0;
  int immutable_memtables = 0;
};

struct StorageOptions {
  std::string path;
  SyncMode sync_mode = SyncMode::kInterval;
  uint64_t sync_interval_us = 1'000'000;
  size_t block_cache_size = 0;  // 0 = engine default
  size_t memtable_size = 0;     // 0 = engine default
  int flush_threads = 0;        // 0 = engine default
  int compaction_threads = 0;   // 0 = engine default
  Compression compression = Compression::kLz4;
};

// Storage backed by TidesDB. The only module that touches the TidesDB C API.
// Thread safety: BeginWrite/BeginRead/BeginSnapshot/Backup/Checkpoint/Compact/Stats may be
// called concurrently from multiple threads (the engine is thread-safe; the background
// compaction scheduler calls Compact from its own thread while transactions run). Each returned
// WriteTxn/ReadView/Iterator is single-threaded: use it from one thread at a time,
// and destroy it before this object. WriteTxn::Commit and Checkpoint are mutually exclusive
// process-wide (D1: TidesDB 10.1.1 split-apply race across memtable rotations on kill -9).
class TidesStorage final : public Storage, public Compactor {
 public:
  // Busy("... locked ...") when another handle or process holds the directory.
  static Result<std::unique_ptr<TidesStorage>> Open(const StorageOptions& options);
  ~TidesStorage() override;
  TidesStorage(const TidesStorage&) = delete;
  TidesStorage& operator=(const TidesStorage&) = delete;

  Result<std::unique_ptr<WriteTxn>> BeginWrite() override;
  Result<std::unique_ptr<ReadView>> BeginRead() override;
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override;

  Status Backup(const std::string& dir);  // consistent, directly-openable copy
  Status Checkpoint() override;           // flush + fsync WAL/vlog/manifest

  Compression compression() const;  // what Open applied (kNone if this build lacks the algorithm)
  // Synchronous whole-CF compaction; drops data covered by range tombstones. Busy if a
  // compaction already holds the CF.
  Status Compact(ColumnFamily cf) override;
  Result<StorageStats> Stats();

 private:
  struct Impl;
  explicit TidesStorage(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace tideskv
