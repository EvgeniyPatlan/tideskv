#include "migrate/slot_eraser.h"

#include <chrono>
#include <string>
#include <thread>

#include "types/slot_erase.h"

namespace tideskv::migrate {
namespace {

constexpr auto kPausePoll = std::chrono::milliseconds(10);

}  // namespace

Status SlotEraser::Commit(const std::vector<encoding::ReplOp>& ops) {
  if (ops.empty()) return Status::Ok();
  for (;;) {
    if (hooks_.stopping && hooks_.stopping()) return Status::IOError("stopped");
    if (!hooks_.begin_write || hooks_.begin_write()) break;
    std::this_thread::sleep_for(kPausePoll);
  }
  const Status st = engine_.WriteInternal(ops);
  if (hooks_.begin_write && hooks_.end_write) hooks_.end_write();
  return st;
}

Status SlotEraser::Erase(cluster::SlotRange range) {
  std::string after;
  for (;;) {
    ExpireDeletes step;
    Status read = engine_.Read([&](ReadView& v, int64_t) {
      return MoveInto(CollectExpireDeletes(v, range.start, range.end, after, kDeletesPerTxn), &step);
    });
    if (!read.ok()) return read;
    if (Status st = Commit(step.ops); !st.ok()) return st;
    if (step.done) break;
    after = std::move(step.resume_after);
  }
  return Commit(SlotRangeDeletes(range.start, range.end));
}

}  // namespace tideskv::migrate
