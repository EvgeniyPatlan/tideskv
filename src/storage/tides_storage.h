#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "storage/storage.h"

namespace tideskv {

enum class SyncMode : uint8_t { kNone, kInterval, kFull };

struct StorageOptions {
  std::string path;
  SyncMode sync_mode = SyncMode::kInterval;
  uint64_t sync_interval_us = 1'000'000;
  size_t block_cache_size = 0;  // 0 = engine default
  size_t memtable_size = 0;     // 0 = engine default
  int flush_threads = 0;        // 0 = engine default
  int compaction_threads = 0;   // 0 = engine default
};

// Storage backed by TidesDB. The only module that touches the TidesDB C API.
// Thread safety: BeginWrite/BeginRead/BeginSnapshot/Backup/Checkpoint may be called
// concurrently from multiple threads (the engine is thread-safe). Each returned
// WriteTxn/ReadView/Iterator is single-threaded: use it from one thread at a time,
// and destroy it before this object.
class TidesStorage final : public Storage {
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
  Status Checkpoint();                     // flush + fsync WAL/vlog/manifest

 private:
  struct Impl;
  explicit TidesStorage(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace tideskv
