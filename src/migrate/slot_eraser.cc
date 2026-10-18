#include "migrate/slot_eraser.h"

#include <chrono>
#include <string>
#include <thread>

#include "types/slot_erase.h"

namespace tideskv::migrate {
namespace {

constexpr auto kPausePoll = std::chrono::milliseconds(10);

// Ends one admitted write however the scope exits (final review M4, as BackgroundWrite): a WriteInternal that
// throws never leaks a CLIENT PAUSE in-flight count.
class AdmittedWrite {
 public:
  explicit AdmittedWrite(const SlotEraser::Hooks& hooks) : hooks_(hooks) {}
  ~AdmittedWrite() {
    if (hooks_.begin_write && hooks_.end_write) hooks_.end_write();
  }
  AdmittedWrite(const AdmittedWrite&) = delete;
  AdmittedWrite& operator=(const AdmittedWrite&) = delete;

 private:
  const SlotEraser::Hooks& hooks_;
};

}  // namespace

Status SlotEraser::Commit(const std::vector<encoding::ReplOp>& ops) {
  if (ops.empty()) return Status::Ok();
  for (;;) {
    if (hooks_.stopping && hooks_.stopping()) return Status::IOError("stopped");
    if (!hooks_.begin_write || hooks_.begin_write()) break;
    std::this_thread::sleep_for(kPausePoll);
  }
  const AdmittedWrite admitted(hooks_);
  return engine_.WriteInternal(ops);
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
