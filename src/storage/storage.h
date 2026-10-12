#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "common/status.h"
#include "storage/column_family.h"

// The engine boundary (spec §3.1). Everything above storage/ talks only to these
// interfaces; TidesStorage and InMemoryStorage implement them and must pass the
// same contract suite (tests/contract).
namespace tideskv {

inline constexpr size_t kMaxRangeBoundSize = 256;  // TidesDB TDB_MAX_RANGE_BOUND_SIZE

struct IterOptions {
  std::string lower;  // inclusive; empty = from the first key
  std::string upper;  // exclusive; empty = to the last key
};

// A new Iterator is unpositioned (Valid() == false) until a Seek* call.
// key()/value() require Valid().
// Empty targets: Seek("") behaves as SeekToFirst(); SeekForPrev("") returns OK with Valid() == false.
// Reaching the end is not an error: positioning calls return OK and Valid() turns false.
// key()/value() remain valid until the next positioning call. An Iterator must be
// destroyed before the ReadView/WriteTxn that created it.
// Cost: TidesStorage enforces the bounds over an unbounded engine iterator, so the call that leaves
// the range (the last Next()/Seek(), or Prev()/SeekForPrev() below `lower`) can step over every
// deleted-but-not-yet-compacted key beyond that bound: O(dead keys past the bound) until compaction
// physically drops them.
class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual Status SeekToFirst() = 0;                         // first key >= lower
  virtual Status SeekToLast() = 0;                          // last key < upper
  virtual Status Seek(std::string_view target) = 0;         // first key >= target
  virtual Status SeekForPrev(std::string_view target) = 0;  // last key <= target
  virtual Status Next() = 0;                                // InvalidArgument if !Valid()
  virtual Status Prev() = 0;                                // InvalidArgument if !Valid()
  virtual bool Valid() const = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
};

class ReadView {
 public:
  virtual ~ReadView() = default;
  // NotFound when absent; InvalidArgument for an empty key.
  virtual Result<std::string> Get(ColumnFamily cf, std::string_view key) = 0;
  virtual Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts = {}) = 0;
};

// Buffered operations take effect in program order, for reads inside the
// transaction and at commit.
// BeginRead views and WriteTxn reads observe commits made after the view/transaction
// was created (read-committed); BeginSnapshot views never do. An Iterator reflects the
// data as of its creation or newer - callers must not rely on either.
// Whether a savepoint still exists after RollbackToSavepoint is unspecified; use unique names.
// Reads observe this transaction's own buffered writes. Destroying an unfinished
// transaction rolls it back. After Commit/Rollback every call returns InvalidArgument.
class WriteTxn : public ReadView {
 public:
  virtual Status Put(ColumnFamily cf, std::string_view key, std::string_view value) = 0;
  virtual Status Delete(ColumnFamily cf, std::string_view key) = 0;
  // O(1) to write. prefix: 1..kMaxRangeBoundSize bytes. A later Put in the same
  // transaction under the prefix survives.
  virtual Status DeletePrefix(ColumnFamily cf, std::string_view prefix) = 0;
  // Deletes [lo, hi). lo: 1..256 bytes; hi: 0..256 bytes, empty = to the end; lo < hi.
  virtual Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) = 0;
  virtual Status Savepoint(std::string_view name) = 0;
  virtual Status RollbackToSavepoint(std::string_view name) = 0;  // NotFound if unknown
  virtual Status Commit() = 0;
  virtual Status Rollback() = 0;
};

// ReadViews and WriteTxns must be destroyed before the Storage that created them.
class Storage {
 public:
  virtual ~Storage() = default;
  virtual Result<std::unique_ptr<WriteTxn>> BeginWrite() = 0;     // read-committed
  virtual Result<std::unique_ptr<ReadView>> BeginRead() = 0;      // latest committed data
  virtual Result<std::unique_ptr<ReadView>> BeginSnapshot() = 0;  // frozen point in time
};

}  // namespace tideskv
