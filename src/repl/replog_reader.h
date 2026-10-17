#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include "common/status.h"
#include "storage/storage.h"

// Tails the replication log from a seq on (M6a D9; M7b D2, D19): the replica senders and the slot migrator.
namespace tideskv::repl {

class ReplogReader {
 public:
  using BatchFn = std::function<Status(uint64_t seq, std::string_view payload)>;
  ReplogReader(Storage& storage, uint64_t next) : storage_(storage), next_(next) {}

  // Hands batches next() .. last to fn in seq order, from a fresh read view (an old iterator does not see
  // newer commits) positioned with Seek (tidesdb-replication constraint 8), until at least max_bytes of
  // payload went out; next() moves past every batch fn accepted. OK with nothing when next() > last.
  // NotFound "replog entry <n> is not retained" when next() <= last and replog[next()] is gone (trimmed,
  // R16, D19). An error from fn stops the read and is returned; next() then still names that batch.
  Status Read(uint64_t last, size_t max_bytes, const BatchFn& fn);
  uint64_t next() const { return next_; }

 private:
  Storage& storage_;
  uint64_t next_;
};

}  // namespace tideskv::repl
