#include "migrate/slot_eraser.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "common/coding.h"
#include "encoding/key_codec.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/replica_stack.h"
#include "types/slot_erase.h"
#include "types/slot_keys.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

// Keys of `cf` in db 0 whose slot is `slot` (bytes 1-2, or 9-10 in the expire CF).
size_t KeysOfSlot(CommandHarness& h, ColumnFamily cf, uint16_t slot) {
  const size_t at = cf == ColumnFamily::kExpire ? 9 : 1;
  size_t n = 0;
  for (const auto& [key, value] : h.Cf(cf)) {
    if (key.size() >= at + 2 && key[0] == 0 && coding::DecodeFixed16BE(key.data() + at) == slot) ++n;
  }
  return n;
}

void SeedSlotB(CommandHarness& h) {
  ASSERT_EQ(h.Run({"SET", "b", "1", "PX", "100000"}), kOk);
  ASSERT_EQ(h.Run({"HSET", "{b}h", "f", "v"}), Int(1));
  ASSERT_EQ(h.Run({"PEXPIRE", "{b}h", "100000"}), Int(1));
  ASSERT_EQ(h.Run({"ZADD", "{b}z", "1", "m"}), Int(1));
  ASSERT_EQ(h.Run({"RPUSH", "{b}l", "a", "b"}), Int(2));
  ASSERT_EQ(h.Run({"SADD", "{b}s", "x"}), Int(1));
  ASSERT_EQ(h.Run({"SET", "foo", "1", "PX", "100000"}), kOk);  // slot 12182: must survive
  ASSERT_EQ(h.Run({"HSET", "{foo}h", "f", "v"}), Int(1));
}

TEST(SlotErase, CollectsTheExpireDeletesOfTheRangeInResumableSteps) {  // D9 step 1
  CommandHarness h;
  for (int i = 0; i < 5; ++i) ASSERT_EQ(h.Run({"SET", "{b}" + std::to_string(i), "v", "PX", "100000"}), kOk);
  ASSERT_EQ(h.Run({"SET", "{b}nottl", "v"}), kOk);
  ASSERT_EQ(h.Run({"SET", "foo", "v", "PX", "100000"}), kOk);
  size_t total = 0;
  std::string after;
  int steps = 0;
  for (bool done = false; !done; ++steps) {
    ExpireDeletes step;
    ASSERT_TRUE(h.services.engine_
                    .Read([&](ReadView& v, int64_t) {
                      return MoveInto(CollectExpireDeletes(v, 3300, 3300, after, 2), &step);
                    })
                    .ok());
    for (const encoding::ReplOp& op : step.ops) {
      EXPECT_EQ(op.type, encoding::ReplOpType::kDelete);
      EXPECT_EQ(op.cf, ColumnFamily::kExpire);
      encoding::ExpireKey ek;
      ASSERT_TRUE(encoding::DecodeExpireKey(op.key, &ek));
      EXPECT_EQ(ek.slot, 3300);
    }
    total += step.ops.size();
    after = step.resume_after;
    done = step.done;
  }
  EXPECT_EQ(total, 5u);
  EXPECT_EQ(steps, 3);
}

TEST(SlotErase, RangeDeletesAreThreeAndBoundTheLastSlot) {  // D9 step 2, the 32-per-commit cap
  const auto ops = SlotRangeDeletes(16380, 16383);
  ASSERT_EQ(ops.size(), 3u);
  EXPECT_EQ(ops[0].cf, ColumnFamily::kMeta);
  EXPECT_EQ(ops[1].cf, ColumnFamily::kSubkey);
  EXPECT_EQ(ops[2].cf, ColumnFamily::kZScore);
  for (const auto& op : ops) {
    EXPECT_EQ(op.type, encoding::ReplOpType::kDeleteRange);
    EXPECT_EQ(op.key, encoding::MetaSlotPrefix(0, 16380));
    EXPECT_EQ(op.value, std::string("\x00\x40\x00", 3));
  }
}

TEST(SlotEraser, ErasesEveryCfOfTheRangeAndNothingElse) {  // D9
  CommandHarness h;
  SeedSlotB(h);
  migrate::SlotEraser eraser(h.services.engine_);
  ASSERT_TRUE(eraser.Erase({3300, 3300}).ok());
  for (ColumnFamily cf :
       {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore, ColumnFamily::kExpire}) {
    EXPECT_EQ(KeysOfSlot(h, cf, 3300), 0u) << ColumnFamilyName(cf);
  }
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(2));
  EXPECT_EQ(h.Run({"GET", "foo"}), Bulk("1"));
  EXPECT_EQ(h.Run({"HGET", "{foo}h", "f"}), Bulk("v"));
  EXPECT_EQ(h.Cf(ColumnFamily::kExpire).size(), 1u);  // foo's
}

TEST(SlotEraser, IsIdempotentAndReplicated) {  // D9: a crash between steps is finished by the next run
  CommandHarness h;
  SeedSlotB(h);
  InMemoryStorage replica_storage;
  ReplicaStack replica(replica_storage);
  migrate::SlotEraser eraser(h.services.engine_);
  ASSERT_TRUE(eraser.Erase({3000, 3400}).ok());
  ASSERT_TRUE(eraser.Erase({3000, 3400}).ok());
  ASSERT_TRUE(ShipAll(h.services.storage_, h.services.repl_log_.last(), replica.engine).ok());
  EXPECT_EQ(DigestOf(replica.engine), DigestOf(h.services.engine_));
}

TEST(SlotEraser, WaitsWhilePausedAndStopsWhenAsked) {  // D9: CLIENT PAUSE holds the cleanup
  CommandHarness h;
  SeedSlotB(h);
  std::atomic<bool> paused{true};
  std::atomic<int> ends{0};
  migrate::SlotEraser::Hooks hooks;
  hooks.begin_write = [&] { return !paused.load(); };
  hooks.end_write = [&] { ++ends; };
  migrate::SlotEraser eraser(h.services.engine_, hooks);
  Status st;
  std::thread t([&] { st = eraser.Erase({3300, 3300}); });
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(KeysOfSlot(h, ColumnFamily::kMeta, 3300), 5u) << "nothing commits while paused";
  paused = false;
  t.join();
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(ends.load(), 2);  // the expire step and the range step
  migrate::SlotEraser::Hooks stopping;
  stopping.begin_write = [] { return false; };
  stopping.stopping = [] { return true; };
  ASSERT_EQ(h.Run({"SET", "{b}again", "1", "PX", "100000"}), kOk);
  EXPECT_EQ(migrate::SlotEraser(h.services.engine_, stopping).Erase({3300, 3300}).message(), "stopped");
}

}  // namespace
}  // namespace tideskv::test
