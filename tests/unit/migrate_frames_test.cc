#include "migrate/migrate_frames.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "encoding/object_codec.h"
#include "support/key_layout_skip.h"
#include "types/object_store.h"

namespace tideskv::migrate {
namespace {

using encoding::Metadata;
using encoding::ReplOp;
using encoding::ReplOpType;
using encoding::ValueType;
constexpr ColumnFamily kMeta = ColumnFamily::kMeta;
constexpr ColumnFamily kSubkey = ColumnFamily::kSubkey;
constexpr ColumnFamily kZScore = ColumnFamily::kZScore;
constexpr ColumnFamily kExpire = ColumnFamily::kExpire;

const cluster::SlotRange kB{3300, 3300};  // the slot of "b"

ReplOp Put(ColumnFamily cf, std::string key, std::string value) {
  return {ReplOpType::kPut, cf, std::move(key), std::move(value)};
}
ReplOp Del(ColumnFamily cf, std::string key) { return {ReplOpType::kDelete, cf, std::move(key), ""}; }
ReplOp DelPrefix(ColumnFamily cf, std::string prefix) {
  return {ReplOpType::kDeletePrefix, cf, std::move(prefix), ""};
}
ReplOp DelRange(ColumnFamily cf, std::string lo, std::string hi) {
  return {ReplOpType::kDeleteRange, cf, std::move(lo), std::move(hi)};
}

struct Counter {
  uint64_t next = 1000;
  OidRemap::Allocate Fn() {
    return [this]() -> Result<uint64_t> { return next++; };
  }
};

Metadata Hash(uint64_t oid, int64_t expire_ms) {
  Metadata m;
  m.type = ValueType::kHash;
  m.oid = oid;
  m.size = 1;
  m.expire_ms = expire_ms;
  return m;
}

TEST(MigrateFrames, ClassifiesKeyspaceOpsBySlot) {
  TIDESKV_SKIP_IF_SLOTLESS();
  EXPECT_EQ(ClassifyOp(Put(kMeta, encoding::MetaKeyFor(0, "b"), "x"), kB), OpClass::kInRange);
  EXPECT_EQ(ClassifyOp(Put(kMeta, encoding::MetaKeyFor(0, "foo"), "x"), kB), OpClass::kOutside);
  EXPECT_EQ(ClassifyOp(Put(kMeta, encoding::MetaKeyFor(3, "b"), "x"), kB), OpClass::kOutside);  // db 3
  EXPECT_EQ(ClassifyOp(Put(kSubkey, encoding::EncodeSubkey(0, 3300, 7, "f"), "v"), kB), OpClass::kInRange);
  EXPECT_EQ(ClassifyOp(Put(kZScore, encoding::EncodeZScoreKey(0, 3300, 7, 1.5, "m"), ""), kB),
            OpClass::kInRange);
  EXPECT_EQ(ClassifyOp(Del(kExpire, encoding::ExpireKeyFor(0, 5000, "b")), kB), OpClass::kInRange);
  EXPECT_EQ(ClassifyOp(Put(kExpire, encoding::ExpireKeyFor(0, 5000, "foo"), ExpireValue(0)), kB),
            OpClass::kOutside);
  EXPECT_EQ(ClassifyOp(DelPrefix(kSubkey, encoding::SubkeyPrefix(0, 3300, 7)), kB), OpClass::kInRange);
  EXPECT_EQ(ClassifyOp(Put(ColumnFamily::kSys, "oid_hwm", "x"), kB), OpClass::kDrop);
  EXPECT_EQ(ClassifyOp(Put(ColumnFamily::kSys, "migration", "x"), kB), OpClass::kDrop);
  const cluster::SlotRange tail{16000, 16383};
  EXPECT_EQ(ClassifyOp(Put(kMeta, encoding::EncodeMetaKey(0, 16383, "k"), "x"), tail), OpClass::kInRange);
  EXPECT_EQ(ClassifyOp(Put(kMeta, encoding::EncodeMetaKey(0, 15999, "k"), "x"), tail), OpClass::kOutside);
}

TEST(MigrateFrames, FlushOpsCancelAndOtherRangeDeletesAreIgnored) {  // D10, Writer's note 2
  TIDESKV_SKIP_IF_SLOTLESS();
  for (ColumnFamily cf : {kMeta, kSubkey, kZScore, kExpire}) {
    EXPECT_EQ(ClassifyOp(DelPrefix(cf, encoding::DbPrefix(0)), kB), OpClass::kFlush);  // FLUSHDB
    EXPECT_EQ(ClassifyOp(DelRange(cf, encoding::DbPrefix(0), encoding::DbPrefix(16)), kB),
              OpClass::kFlush);                                                            // FLUSHALL
    EXPECT_EQ(ClassifyOp(DelRange(cf, std::string("\x00", 1), ""), kB), OpClass::kFlush);  // a CF wipe
    EXPECT_EQ(ClassifyOp(DelPrefix(cf, encoding::DbPrefix(2)), kB), OpClass::kOutside);    // another db
  }
  // The eraser of another range (Task 12's cleanup, running during this job) is not a flush.
  EXPECT_EQ(
      ClassifyOp(DelRange(kMeta, encoding::MetaSlotPrefix(0, 100), encoding::MetaSlotPrefix(0, 201)), kB),
      OpClass::kOutside);
  EXPECT_EQ(
      ClassifyOp(DelRange(kSubkey, encoding::MetaSlotPrefix(0, 3301), encoding::MetaSlotPrefix(0, 3400)), kB),
      OpClass::kOutside);
  EXPECT_EQ(
      ClassifyOp(DelRange(kZScore, encoding::MetaSlotPrefix(0, 3000), encoding::MetaSlotPrefix(0, 3301)), kB),
      OpClass::kFlush);
  bool flush = false;
  const std::vector<ReplOp> in =
      FilterBatch({Put(kMeta, encoding::MetaKeyFor(0, "foo"), "1"),
                   Put(kMeta, encoding::MetaKeyFor(0, "b"), "2"), DelPrefix(kMeta, encoding::DbPrefix(0))},
                  kB, &flush);
  EXPECT_TRUE(flush);
  ASSERT_EQ(in.size(), 1u);
  EXPECT_EQ(in[0].value, "2");
}

TEST(MigrateFrames, RemapsEveryOidSite) {  // D2 step 4
  TIDESKV_SKIP_IF_SLOTLESS();
  Counter c;
  OidRemap remap(c.Fn());
  auto sub = RemapOp(Put(kSubkey, encoding::EncodeSubkey(0, 3300, 42, "f"), "v"), remap);
  ASSERT_TRUE(sub.ok() && sub->has_value());
  EXPECT_EQ((*sub)->key, encoding::EncodeSubkey(0, 3300, 1000, "f"));
  EXPECT_EQ((*sub)->value, "v");
  auto z = RemapOp(Put(kZScore, encoding::EncodeZScoreKey(0, 3300, 42, 2.5, "m"), ""), remap);
  EXPECT_EQ((*z)->key, encoding::EncodeZScoreKey(0, 3300, 1000, 2.5, "m"));
  auto meta = RemapOp(Put(kMeta, encoding::MetaKeyFor(0, "b"), EncodeMetadata(Hash(42, 9000))), remap);
  auto decoded = encoding::DecodeMetadata((*meta)->value);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->meta.oid, 1000u);
  EXPECT_EQ(decoded->meta.expire_ms, 9000);
  Metadata list;
  list.type = ValueType::kList;
  list.oid = 43;
  list.size = 2;
  list.head = encoding::kListInitialIndex - 1;
  list.tail = encoding::kListInitialIndex;
  auto lm = RemapOp(Put(kMeta, encoding::MetaKeyFor(0, "{b}l"), EncodeMetadata(list)), remap);
  auto ld = encoding::DecodeMetadata((*lm)->value);
  EXPECT_EQ(ld->meta.oid, 1001u);
  EXPECT_EQ(ld->meta.head, list.head);
  EXPECT_EQ(ld->meta.tail, list.tail);
  auto ex = RemapOp(Put(kExpire, encoding::ExpireKeyFor(0, 9000, "b"), ExpireValue(42)), remap);
  EXPECT_EQ((*ex)->key, encoding::ExpireKeyFor(0, 9000, "b"));
  EXPECT_EQ((*ex)->value, ExpireValue(1000));
  auto dp = RemapOp(DelPrefix(kSubkey, encoding::SubkeyPrefix(0, 3300, 42)), remap);
  EXPECT_EQ((*dp)->key, encoding::SubkeyPrefix(0, 3300, 1000));
  auto dz = RemapOp(DelPrefix(kZScore, encoding::ZScorePrefix(0, 3300, 43)), remap);
  EXPECT_EQ((*dz)->key, encoding::ZScorePrefix(0, 3300, 1001));
  const std::string str = EncodeMetadata(Metadata{}, "payload");
  auto s = RemapOp(Put(kMeta, encoding::MetaKeyFor(0, "{b}s"), str), remap);
  EXPECT_EQ((*s)->value, str);
  auto se = RemapOp(Put(kExpire, encoding::ExpireKeyFor(0, 9000, "{b}s"), ExpireValue(0)), remap);
  EXPECT_EQ((*se)->value, ExpireValue(0));
  auto del = RemapOp(Del(kSubkey, encoding::EncodeSubkey(0, 3300, 42, "f")), remap);
  EXPECT_EQ((*del)->key, encoding::EncodeSubkey(0, 3300, 1000, "f"));
  auto exdel = RemapOp(Del(kExpire, encoding::ExpireKeyFor(0, 9000, "b")), remap);
  EXPECT_EQ((*exdel)->key, encoding::ExpireKeyFor(0, 9000, "b"));
  EXPECT_EQ(remap.size(), 2u);  // 42 and 43: strings take none
  EXPECT_EQ(c.next, 1002u);
}

TEST(MigrateFrames, ADelPrefixOfAnUnseenOidIsDropped) {
  TIDESKV_SKIP_IF_SLOTLESS();
  Counter c;
  OidRemap remap(c.Fn());
  auto dp = RemapOp(DelPrefix(kSubkey, encoding::SubkeyPrefix(0, 3300, 77)), remap);
  ASSERT_TRUE(dp.ok());
  EXPECT_FALSE(dp->has_value());
  EXPECT_EQ(remap.size(), 0u);
  EXPECT_EQ(c.next, 1000u);
}

TEST(MigrateFrames, AllocationIsLazyAndOrderIndependent) {  // a batch puts subkeys before the meta
  TIDESKV_SKIP_IF_SLOTLESS();
  Counter c;
  OidRemap remap(c.Fn());
  auto sub = RemapOp(Put(kSubkey, encoding::EncodeSubkey(0, 3300, 42, "f"), "v"), remap);
  auto meta = RemapOp(Put(kMeta, encoding::MetaKeyFor(0, "b"), EncodeMetadata(Hash(42, 0))), remap);
  EXPECT_EQ((*sub)->key, encoding::EncodeSubkey(0, 3300, 1000, "f"));
  EXPECT_EQ(encoding::DecodeMetadata((*meta)->value)->meta.oid, 1000u);
  EXPECT_EQ(remap.Find(42), std::optional<uint64_t>(1000));
  EXPECT_FALSE(remap.Find(43).has_value());
}

TEST(MigrateFrames, AFailedAllocationFailsTheOp) {
  TIDESKV_SKIP_IF_SLOTLESS();
  OidRemap remap([]() -> Result<uint64_t> { return Status::IOError("lease failed"); });
  auto sub = RemapOp(Put(kSubkey, encoding::EncodeSubkey(0, 3300, 42, "f"), "v"), remap);
  EXPECT_EQ(sub.status().code(), StatusCode::kIOError);
}

TEST(MigrateFrames, DerivesTheExpireEntryOfAMetaWithATtl) {  // D2 step 5
  TIDESKV_SKIP_IF_SLOTLESS();
  auto coll = DerivedExpire(Put(kMeta, encoding::MetaKeyFor(0, "b"), EncodeMetadata(Hash(1000, 9000))));
  ASSERT_TRUE(coll.ok() && coll->has_value());
  EXPECT_EQ(**coll, Put(kExpire, encoding::ExpireKeyFor(0, 9000, "b"), ExpireValue(1000)));
  Metadata str;
  str.expire_ms = 5000;
  auto s = DerivedExpire(Put(kMeta, encoding::MetaKeyFor(0, "{b}s"), EncodeMetadata(str, "v")));
  EXPECT_EQ(**s, Put(kExpire, encoding::ExpireKeyFor(0, 5000, "{b}s"), ExpireValue(0)));
  EXPECT_FALSE(
      DerivedExpire(Put(kMeta, encoding::MetaKeyFor(0, "b"), EncodeMetadata(Hash(1, 0))))->has_value());
  EXPECT_FALSE(DerivedExpire(Del(kMeta, encoding::MetaKeyFor(0, "b")))->has_value());
  EXPECT_FALSE(DerivedExpire(Put(kSubkey, encoding::EncodeSubkey(0, 3300, 1, "f"), "v"))->has_value());
  EXPECT_EQ(DerivedExpire(Put(kMeta, encoding::MetaKeyFor(0, "b"), "short")).status().code(),
            StatusCode::kCorruption);
}

TEST(MigrateFrames, TheImporterAcceptsOnlyInRangeKeyspaceOps) {  // D2 step 2
  TIDESKV_SKIP_IF_SLOTLESS();
  EXPECT_TRUE(CheckImportOp(Put(kMeta, encoding::MetaKeyFor(0, "b"), "x"), kB).ok());
  EXPECT_TRUE(CheckImportOp(Del(kSubkey, encoding::EncodeSubkey(0, 3300, 1, "f")), kB).ok());
  EXPECT_TRUE(CheckImportOp(Put(kExpire, encoding::ExpireKeyFor(0, 1, "b"), ExpireValue(1)), kB).ok());
  EXPECT_TRUE(CheckImportOp(DelPrefix(kZScore, encoding::ZScorePrefix(0, 3300, 1)), kB).ok());
  for (const ReplOp& bad :
       {Put(kMeta, encoding::MetaKeyFor(0, "foo"), "x"), Put(ColumnFamily::kSys, "oid_hwm", "x"),
        DelRange(kMeta, encoding::MetaSlotPrefix(0, 3300), encoding::MetaSlotPrefix(0, 3301)),
        DelPrefix(kMeta, encoding::DbPrefix(0)), Put(ColumnFamily::kReplog, encoding::ReplogKey(1), "x")}) {
    EXPECT_EQ(CheckImportOp(bad, kB).code(), StatusCode::kCorruption);
  }
}

TEST(MigrateFrames, ChunksRespectTheByteBudgetAndRoundTrip) {  // D3
  TIDESKV_SKIP_IF_SLOTLESS();
  std::vector<ReplOp> ops;
  for (int i = 0; i < 10; ++i) {  // each 20 + 14 + 16 = 50 budget bytes
    ops.push_back(Put(kMeta, std::string(20, static_cast<char>('a' + i)), std::string(14, 'v')));
  }
  ops.push_back(Put(kMeta, std::string(20, 'z'), std::string(1000, 'v')));  // larger than a chunk: alone
  const std::vector<std::string> chunks = EncodeChunks(ops, 150);
  ASSERT_EQ(chunks.size(), 5u);  // 3 + 3 + 3 + 1 small ops, then the large one alone
  std::vector<ReplOp> back;
  for (const std::string& chunk : chunks) {
    auto decoded = DecodeChunk(chunk);
    ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
    back.insert(back.end(), decoded->begin(), decoded->end());
  }
  EXPECT_EQ(back, ops);
  EXPECT_EQ(DecodeChunk(std::string("garbage")).status().code(), StatusCode::kCorruption);
  std::string corrupted = chunks[0];
  corrupted[corrupted.size() / 2] ^= 0x5a;
  EXPECT_EQ(DecodeChunk(corrupted).status().code(), StatusCode::kCorruption);
  EXPECT_TRUE(EncodeChunks({}, 150).empty());
}

}  // namespace
}  // namespace tideskv::migrate
