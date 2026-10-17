#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "exec/engine.h"
#include "exec/slot_fence.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/replica_stack.h"
#include "types/errors.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

const uint16_t kSlotB = KeySlot("b");  // 3300

Engine::WriteFn PutString(const std::string& key, const std::string& value) {
  return [key, value](WriteTxn& t, int64_t) {
    return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, key), encoding::EncodeMetadata({}, value));
  };
}

TEST(EngineFence, WritesToAFencedSlotAnswerTryagainAndOthersRun) {  // D4
  CommandHarness h;
  Engine& e = h.services.engine_;
  EXPECT_EQ(e.FenceSlots(kSlotB, kSlotB), h.services.repl_log_.last());
  EXPECT_TRUE(e.SlotFenced(kSlotB));
  EXPECT_FALSE(e.SlotFenced(kSlotB + 1));
  EXPECT_EQ(e.Write(0, {"b"}, PutString("b", "1")).message(), kErrTryAgain);
  EXPECT_EQ(e.Write(0, {"{b}x"}, PutString("{b}x", "1")).message(), kErrTryAgain);  // the hash tag counts
  EXPECT_TRUE(e.Write(0, {"foo"}, PutString("foo", "1")).ok());
  EXPECT_EQ(h.Run({"SET", "b", "1"}), Err(kErrTryAgain));  // through Dispatch: the Engine refuses
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);                    // reads are never fenced
  e.UnfenceSlots(kSlotB, kSlotB);
  EXPECT_EQ(h.Run({"SET", "b", "1"}), kOk);
}

// The barrier (CI §3.2): a write that already holds the shared gate when FenceSlots starts commits before
// FenceSlots returns, and its seq is inside the final delta.
TEST(EngineFence, AWriteInsideTheGateCommitsBeforeFenceSlotsReturns) {
  CommandHarness h;
  Engine& e = h.services.engine_;
  std::promise<void> inside;
  std::promise<void> release;
  const std::shared_future<void> released = release.get_future().share();
  std::thread writer([&] {
    const Status st = e.Write(0, {"b"}, [&](WriteTxn& t, int64_t) {
      inside.set_value();  // past AcquireShared and the fence check
      released.wait();
      return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"), encoding::EncodeMetadata({}, "v"));
    });
    EXPECT_TRUE(st.ok()) << st.ToString();
  });
  inside.get_future().wait();
  const uint64_t before = h.services.repl_log_.last();
  std::atomic<bool> fenced{false};
  uint64_t fence_seq = 0;
  std::thread fencer([&] {
    fence_seq = e.FenceSlots(kSlotB, kSlotB);
    fenced = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(fenced.load()) << "the barrier must wait for the write that already holds the gate";
  release.set_value();
  writer.join();
  fencer.join();
  EXPECT_EQ(fence_seq, before + 1) << "the in-flight write must be inside the final delta";
  EXPECT_EQ(fence_seq, h.services.repl_log_.last());
}

// A write that reaches the gate after the barrier started waiting queues behind it (writer priority) and is
// refused: nothing to a fenced slot commits after the returned seq.
TEST(EngineFence, AWriteQueuedBehindTheBarrierIsRefusedAndNothingCommitsAfterIt) {
  CommandHarness h;
  Engine& e = h.services.engine_;
  auto batch = e.BeginBatch();  // holds the gate exclusively, like an EXEC in progress
  ASSERT_TRUE(batch.ok());
  uint64_t fence_seq = 0;
  std::thread fencer([&] { fence_seq = e.FenceSlots(kSlotB, kSlotB); });
  std::this_thread::sleep_for(50ms);  // the fencer now waits for the gate
  Status late;
  std::thread writer([&] { late = e.Write(0, {"b"}, PutString("b", "late")); });
  std::this_thread::sleep_for(50ms);  // the writer now waits behind the fencer
  batch->reset();
  fencer.join();
  writer.join();
  EXPECT_EQ(late.message(), kErrTryAgain);
  EXPECT_EQ(h.services.repl_log_.last(), fence_seq);
}

TEST(EngineFence, ABatchWriteToAFencedSlotIsRefusedAndTheBatchGoesOn) {  // the EXEC/script backstop
  CommandHarness h;
  Engine& e = h.services.engine_;
  e.FenceSlots(kSlotB, kSlotB);
  auto batch = e.BeginBatch();
  ASSERT_TRUE(batch.ok());
  EXPECT_EQ(e.Write(0, {"b"}, PutString("b", "1")).message(), kErrTryAgain);
  EXPECT_TRUE(e.Write(0, {"foo"}, PutString("foo", "1")).ok());
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_EQ(h.Cf(ColumnFamily::kMeta).count(encoding::MetaKeyFor(0, "b")), 0u);
  EXPECT_EQ(h.Cf(ColumnFamily::kMeta).count(encoding::MetaKeyFor(0, "foo")), 1u);
}

// Writer's note 1: FLUSH* is keyless and unfenced (Kvrocks); a replica's apply is never fenced (D4).
TEST(EngineFence, FlushesAndReplicaAppliesBypassTheFence) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"SET", "b", "1"}), kOk);
  Engine& e = h.services.engine_;
  e.FenceSlots(0, kSlotCount - 1);
  EXPECT_EQ(h.Run({"FLUSHDB"}), kOk);
  InMemoryStorage replica_storage;
  ReplicaStack replica(replica_storage);
  replica.engine.FenceSlots(kSlotB, kSlotB);
  EXPECT_TRUE(ShipAll(h.services.storage_, h.services.repl_log_.last(), replica.engine).ok());
}

TEST(EngineFence, UnfenceAndAssignReplaceTheFencedSet) {
  CommandHarness h;
  Engine& e = h.services.engine_;
  e.FenceSlots(10, 20);
  e.UnfenceSlots(15, kSlotCount - 1);
  EXPECT_TRUE(e.SlotFenced(14));
  EXPECT_FALSE(e.SlotFenced(15));
  SlotBits only;
  only.set(kSlotCount - 1);
  e.AssignFence(only);
  EXPECT_FALSE(e.SlotFenced(10));
  EXPECT_TRUE(e.SlotFenced(kSlotCount - 1));
  EXPECT_EQ(e.FencedSlots(), only);
  e.AssignFence({});
  EXPECT_EQ(h.Run({"SET", "b", "1"}), kOk);
}

TEST(SlotFence, AnyTracksTheBitsAndAnyKeyHashesKeys) {
  SlotFence f;
  EXPECT_FALSE(f.any());
  EXPECT_FALSE(f.AnyKey({"b"}));
  SlotBits bits;
  bits.set(kSlotB);
  f.Assign(bits);
  EXPECT_TRUE(f.any());
  EXPECT_TRUE(f.Test(kSlotB));
  EXPECT_TRUE(f.AnyKey({"foo", "b"}));
  EXPECT_FALSE(f.AnyKey({"foo"}));
  EXPECT_EQ(f.Bits(), bits);
  f.Assign({});
  EXPECT_FALSE(f.any());
}

}  // namespace
}  // namespace tideskv::test
