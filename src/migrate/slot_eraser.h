#pragma once
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "cluster/topology.h"
#include "common/status.h"
#include "encoding/replog_codec.h"
#include "exec/engine.h"

// The range eraser (M7b D9): the source's cleanup, the target's IMPORT start/fail/link loss, FLUSHSLOTS.
namespace tideskv::migrate {

class SlotEraser {
 public:
  static constexpr size_t kDeletesPerTxn = 1024;
  struct Hooks {
    std::function<bool()> begin_write;  // CLIENT PAUSE (D9): false = paused, asked again every 10 ms
    std::function<void()> end_write;    // after each admitted txn (only with begin_write)
    std::function<bool()> stopping;     // checked before each txn and while paused
  };

  explicit SlotEraser(Engine& engine, Hooks hooks = {}) : engine_(engine), hooks_(std::move(hooks)) {}

  // The range's expire entries in txns of at most kDeletesPerTxn deletes, then one txn of the three range
  // deletes; every txn through Engine::WriteInternal (replicated). Idempotent. IOError("stopped") once
  // stopping() holds; otherwise the first failure (nothing after it is written).
  Status Erase(cluster::SlotRange range);

 private:
  Status Commit(const std::vector<encoding::ReplOp>& ops);
  Engine& engine_;
  Hooks hooks_;
};

}  // namespace tideskv::migrate
