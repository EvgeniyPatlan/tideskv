#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "encoding/key_layout.h"
#include "encoding/object_codec.h"
#include "support/command_harness.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Int;
using test::kOk;

// Copying entries to the new slot's prefix happens only in the slotted layout.
struct RenameCollection : ::testing::Test {
  void SetUp() override {
    ASSERT_NE(KeySlot("src"), KeySlot("dst"));
    ASSERT_EQ(KeySlot("{t}a"), KeySlot("{t}b"));
  }
  const encoding::ScopedKeyLayout slotted{encoding::KeyLayout::kSlotted};
  CommandHarness h;
};

TEST(RenameCollectionSlotless, RenameLeavesEveryEntryInPlace) {  // no slot in the keys: O(1) for any key
  const encoding::ScopedKeyLayout slotless(encoding::KeyLayout::kSlotless);
  CommandHarness h;
  EXPECT_EQ(h.Run({"HSET", "src", "f", "v", "g", "w"}), Int(2));
  EXPECT_EQ(h.Run({"ZADD", "zsrc", "1", "a", "2", "b"}), Int(2));
  const auto subkeys = h.Cf(ColumnFamily::kSubkey);
  const auto scores = h.Cf(ColumnFamily::kZScore);
  EXPECT_EQ(h.Run({"RENAME", "src", "dst"}), kOk);
  EXPECT_EQ(h.Run({"RENAME", "zsrc", "zdst"}), kOk);
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey), subkeys);
  EXPECT_EQ(h.Cf(ColumnFamily::kZScore), scores);
  EXPECT_EQ(h.Run({"HGET", "dst", "g"}), Bulk("w"));
  EXPECT_EQ(h.Run({"ZSCORE", "zdst", "b"}), Bulk("2"));
}

TEST_F(RenameCollection, SetWithTtlKeepsMembersAndDeadline) {
  EXPECT_EQ(h.Run({"SADD", "src", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"PEXPIRE", "src", "9000"}), Int(1));
  EXPECT_EQ(h.Run({"RENAME", "src", "dst"}), kOk);
  EXPECT_EQ(h.Run({"SMEMBERS", "dst"}), Arr({Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"PTTL", "dst"}), Int(9000));
  EXPECT_EQ(h.Cf(ColumnFamily::kExpire).size(), 1u);
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 2u);
}

TEST_F(RenameCollection, RenamenxOntoAnExistingKeyMovesNothing) {
  EXPECT_EQ(h.Run({"HSET", "src", "f", "v"}), Int(1));
  EXPECT_EQ(h.Run({"SADD", "dst", "m"}), Int(1));
  EXPECT_EQ(h.Run({"RENAMENX", "src", "dst"}), Int(0));
  EXPECT_EQ(h.Run({"HGET", "src", "f"}), Bulk("v"));
  EXPECT_EQ(h.Run({"SMEMBERS", "dst"}), Arr({Bulk("m")}));
}

TEST_F(RenameCollection, SameSlotRenameLeavesSubkeysInPlace) {
  EXPECT_EQ(h.Run({"HSET", "{t}a", "f", "v"}), Int(1));
  const auto before = h.Cf(ColumnFamily::kSubkey);
  EXPECT_EQ(h.Run({"RENAME", "{t}a", "{t}b"}), kOk);
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey), before);  // O(1): same slot, same oid, same keys
}

void FillHash(CommandHarness& h, const std::string& key, int n) {
  for (int i = 0; i < n; i += 100) {
    ArgList argv = {"HSET", key};
    for (int j = i; j < i + 100 && j < n; ++j) {
      argv.push_back("f" + std::to_string(j));
      argv.push_back(std::to_string(j));
    }
    EXPECT_EQ(h.Run(argv), Int(std::min(100, n - i)));
  }
}

// 2048 = two full copy batches: the last pass reads a full batch, so one more (empty) pass runs.
TEST_F(RenameCollection, HashOfExactlyTwoCopyBatchesMovesCompletely) {
  FillHash(h, "src", 2048);
  EXPECT_EQ(h.Run({"RENAME", "src", "dst"}), kOk);
  EXPECT_EQ(h.Run({"HLEN", "dst"}), Int(2048));
  EXPECT_EQ(h.Run({"HGET", "dst", "f0"}), Bulk("0"));
  EXPECT_EQ(h.Run({"HGET", "dst", "f999"}), Bulk("999"));  // the last field in byte order
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 2048u);
  const std::string to = encoding::SubkeyPrefix(0, KeySlot("dst"), 0).substr(0, 3);  // [db][slot]
  for (const auto& [key, value] : h.Cf(ColumnFamily::kSubkey)) EXPECT_EQ(key.substr(0, 3), to);
}

TEST_F(RenameCollection, LargeHashCrossesSeveralCopyBatches) {
  for (int i = 0; i < 3000; i += 100) {
    ArgList argv = {"HSET", "src"};
    for (int j = i; j < i + 100; ++j) {
      argv.push_back("f" + std::to_string(j));
      argv.push_back(std::to_string(j));
    }
    EXPECT_EQ(h.Run(argv), Int(100));
  }
  EXPECT_EQ(h.Run({"RENAME", "src", "dst"}), kOk);
  EXPECT_EQ(h.Run({"HLEN", "dst"}), Int(3000));
  EXPECT_EQ(h.Run({"HGET", "dst", "f2999"}), Bulk("2999"));
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 3000u);
}

TEST_F(RenameCollection, ZsetEntriesMoveToo) {
  // No ZSET commands until M4: seed the layout directly (meta + subkey + zscore).
  encoding::Metadata meta;
  meta.type = encoding::ValueType::kZSet;
  meta.oid = 950;
  meta.size = 1;
  h.SeedMeta(0, "src", meta);
  {
    auto txn = h.services.storage_.BeginWrite();
    ASSERT_TRUE(
        (*txn)->Put(ColumnFamily::kSubkey, encoding::EncodeSubkey(0, KeySlot("src"), 950, "m"), "x").ok());
    ASSERT_TRUE(
        (*txn)
            ->Put(ColumnFamily::kZScore, encoding::EncodeZScoreKey(0, KeySlot("src"), 950, 1.5, "m"), "")
            .ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  EXPECT_EQ(h.Run({"RENAME", "src", "dst"}), kOk);
  const auto& z = h.Cf(ColumnFamily::kZScore);
  ASSERT_EQ(z.size(), 1u);
  encoding::ZScoreKey zk;
  ASSERT_TRUE(encoding::DecodeZScoreKey(z.begin()->first, &zk));
  EXPECT_EQ(zk.slot, KeySlot("dst"));
  EXPECT_EQ(zk.score, 1.5);
  EXPECT_EQ(zk.member, "m");
}

}  // namespace
}  // namespace tideskv
