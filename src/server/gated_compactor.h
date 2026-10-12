#pragma once
#include <functional>
#include <utility>

#include "storage/compactor.h"

namespace tideskv {

// Runs a Compactor's Checkpoint while the engine is quiesced: a flush that lands while snapshot
// iterators are being built can leave them without a flushed range tombstone (FLUSHDB/FLUSHALL),
// so they briefly show keys that were deleted. CompactRange runs unquiesced.
class GatedCompactor final : public Compactor {
 public:
  using Quiesce = std::function<Status(const std::function<Status()>&)>;
  GatedCompactor(Compactor& inner, Quiesce quiesce) : inner_(inner), quiesce_(std::move(quiesce)) {}
  Status Checkpoint() override {
    return quiesce_([this] { return inner_.Checkpoint(); });
  }
  Status CompactRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
    return inner_.CompactRange(cf, lo, hi);
  }

 private:
  Compactor& inner_;
  Quiesce quiesce_;
};

}  // namespace tideskv
