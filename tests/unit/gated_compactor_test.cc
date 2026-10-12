#include "server/gated_compactor.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace tideskv {
namespace {

class RecordingCompactor final : public Compactor {
 public:
  Status Checkpoint() override {
    log.push_back(quiesced ? "checkpoint(quiesced)" : "checkpoint(running)");
    return checkpoint_status;
  }
  Status CompactRange(ColumnFamily, std::string_view, std::string_view) override {
    log.push_back(quiesced ? "compact(quiesced)" : "compact(running)");
    return Status::Ok();
  }
  bool quiesced = false;
  Status checkpoint_status = Status::Ok();
  std::vector<std::string> log;
};

TEST(GatedCompactor, CheckpointRunsInsideTheQuiesce) {
  RecordingCompactor inner;
  GatedCompactor gated(inner, [&](const std::function<Status()>& fn) {
    inner.quiesced = true;
    const Status st = fn();
    inner.quiesced = false;
    return st;
  });
  EXPECT_TRUE(gated.Checkpoint().ok());
  EXPECT_TRUE(gated.CompactRange(ColumnFamily::kMeta, "", "").ok());
  EXPECT_EQ(inner.log, (std::vector<std::string>{"checkpoint(quiesced)", "compact(running)"}));
}

TEST(GatedCompactor, CheckpointStatusPassesThrough) {
  RecordingCompactor inner;
  inner.checkpoint_status = Status::Busy("flush queue not drained");
  GatedCompactor gated(inner, [](const std::function<Status()>& fn) { return fn(); });
  EXPECT_EQ(gated.Checkpoint().code(), StatusCode::kBusy);
}

}  // namespace
}  // namespace tideskv
