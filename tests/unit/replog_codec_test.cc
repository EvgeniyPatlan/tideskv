#include "encoding/replog_codec.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "common/coding.h"
#include "common/crc32c.h"

namespace tideskv::encoding {
namespace {

std::vector<ReplOp> ThreeOps() {
  return {{ReplOpType::kPut, ColumnFamily::kMeta, "key", "value"},
          {ReplOpType::kDelete, ColumnFamily::kSubkey, "sub", ""},
          {ReplOpType::kDeleteRange, ColumnFamily::kExpire, "lo", "hi"}};
}

// Re-seals a mutated body with a valid trailing CRC so only the field under test is wrong.
std::string Reseal(std::string body_without_crc) {
  coding::PutFixed32BE(&body_without_crc, Crc32c(body_without_crc));
  return body_without_crc;
}

std::string WithoutCrc(const std::string& enc) { return enc.substr(0, enc.size() - 4); }

TEST(ReplogCodec, KeyIsBigEndianSeq) {
  EXPECT_EQ(ReplogKey(1), std::string("\0\0\0\0\0\0\0\x01", 8));
  uint64_t s = 0;
  ASSERT_TRUE(DecodeReplogKey(ReplogKey(0x0102030405060708ULL), &s));
  EXPECT_EQ(s, 0x0102030405060708ULL);
  EXPECT_FALSE(DecodeReplogKey("short", &s));
  EXPECT_LT(ReplogKey(255), ReplogKey(256));  // byte order = numeric order
}

TEST(ReplogCodec, RoundTripsEveryOpType) {
  const std::vector<ReplOp> ops = {{ReplOpType::kPut, ColumnFamily::kMeta, "k", "v"},
                                   {ReplOpType::kDelete, ColumnFamily::kSubkey, std::string("a\0b", 3), ""},
                                   {ReplOpType::kDeletePrefix, ColumnFamily::kZScore, "pre", ""},
                                   {ReplOpType::kDeleteRange, ColumnFamily::kExpire, "lo", "hi"},
                                   {ReplOpType::kDeleteRange, ColumnFamily::kMeta, "lo", ""},
                                   {ReplOpType::kPut, ColumnFamily::kSys, "oid_hwm", std::string(8, '\x01')}};
  const std::string enc = EncodeReplogBatch(ops, 1700000000123, 1000);
  auto dec = DecodeReplogBatch(enc);
  ASSERT_TRUE(dec.ok()) << dec.status().ToString();
  EXPECT_EQ(dec->ops, ops);
  EXPECT_EQ(dec->timestamp_ms, 1700000000123);
  EXPECT_EQ(dec->cum_bytes, 1000 + enc.size());
  int64_t ts = 0;
  uint64_t cum = 0;
  ASSERT_TRUE(PeekReplogHeader(enc, &ts, &cum));
  EXPECT_EQ(ts, 1700000000123);
  EXPECT_EQ(cum, 1000 + enc.size());
}

TEST(ReplogCodec, EmptyBatchRoundTrips) {
  const std::string enc = EncodeReplogBatch({}, -5, 0);
  EXPECT_EQ(enc.size(), kReplogHeaderSize + 4);
  auto dec = DecodeReplogBatch(enc);
  ASSERT_TRUE(dec.ok()) << dec.status().ToString();
  EXPECT_TRUE(dec->ops.empty());
  EXPECT_EQ(dec->timestamp_ms, -5);
  EXPECT_EQ(dec->cum_bytes, enc.size());
}

TEST(ReplogCodec, PeekRejectsShortInput) {
  int64_t ts = 0;
  uint64_t cum = 0;
  EXPECT_FALSE(PeekReplogHeader("", &ts, &cum));
  EXPECT_FALSE(PeekReplogHeader(std::string(kReplogHeaderSize - 1, '\x01'), &ts, &cum));
}

TEST(ReplogCodec, RejectsAnySingleBitFlip) {
  const std::string enc = EncodeReplogBatch(ThreeOps(), 42, 7);
  for (size_t i = 0; i < enc.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string bad = enc;
      bad[i] = static_cast<char>(bad[i] ^ (1 << bit));
      auto r = DecodeReplogBatch(bad);
      ASSERT_FALSE(r.ok()) << "byte " << i << " bit " << bit;
      EXPECT_EQ(r.status().code(), StatusCode::kCorruption);
    }
  }
}

TEST(ReplogCodec, RejectsTruncationAndTrailingBytes) {
  const std::string enc = EncodeReplogBatch(ThreeOps(), 42, 7);
  for (size_t len = 0; len < enc.size(); ++len) {
    auto r = DecodeReplogBatch(std::string_view(enc).substr(0, len));
    ASSERT_FALSE(r.ok()) << len;
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption);
  }
  EXPECT_EQ(DecodeReplogBatch(enc + "x").status().code(), StatusCode::kCorruption);
}

TEST(ReplogCodec, RejectsStructuralDefectsEvenWithValidCrc) {
  const std::string enc = EncodeReplogBatch(ThreeOps(), 42, 7);
  const std::string body = WithoutCrc(enc);
  auto corrupt = [](const std::string& s) {
    return DecodeReplogBatch(s).status().code() == StatusCode::kCorruption;
  };

  std::string v2 = body;
  v2[0] = 2;
  EXPECT_TRUE(corrupt(Reseal(v2)));

  std::string type0 = body;
  type0[kReplogHeaderSize] = 0;
  EXPECT_TRUE(corrupt(Reseal(type0)));
  std::string type5 = body;
  type5[kReplogHeaderSize] = 5;
  EXPECT_TRUE(corrupt(Reseal(type5)));

  std::string replog_cf = body;
  replog_cf[kReplogHeaderSize + 1] = static_cast<char>(ColumnFamily::kReplog);
  EXPECT_TRUE(corrupt(Reseal(replog_cf)));
  std::string big_cf = body;
  big_cf[kReplogHeaderSize + 1] = static_cast<char>(kColumnFamilyCount);
  EXPECT_TRUE(corrupt(Reseal(big_cf)));

  std::string many = body;  // op_count far beyond what the bytes can hold
  many.replace(1 + 8 + 8, 4, std::string("\xff\xff\xff\xff", 4));
  EXPECT_TRUE(corrupt(Reseal(many)));

  std::string fewer = body;  // op_count smaller than the ops present -> trailing bytes
  fewer[kReplogHeaderSize - 1] = 2;
  EXPECT_TRUE(corrupt(Reseal(fewer)));

  std::string longlen = body;  // key length overruns the batch
  longlen[kReplogHeaderSize + 2] = 100;
  EXPECT_TRUE(corrupt(Reseal(longlen)));
}

TEST(ReplogCodec, RejectsNonMinimalVarint) {
  // One PUT whose key length (0) is spelled as the two-byte varint 0x80 0x00.
  std::string body = EncodeReplogBatch({{ReplOpType::kPut, ColumnFamily::kMeta, "", ""}}, 1, 0);
  body = WithoutCrc(body);
  body.insert(kReplogHeaderSize + 2, std::string("\x80", 1));
  EXPECT_EQ(DecodeReplogBatch(Reseal(body)).status().code(), StatusCode::kCorruption);
}

rc::Gen<ReplOp> GenOp() {
  return rc::gen::apply(
      [](int type, int cf, std::string key, std::string value) {
        return ReplOp{static_cast<ReplOpType>(1 + type), static_cast<ColumnFamily>(cf), std::move(key),
                      std::move(value)};
      },
      rc::gen::inRange(0, 4), rc::gen::element(0, 1, 2, 3, 5),
      rc::gen::map(rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>()),
                   [](const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }),
      rc::gen::map(rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>()),
                   [](const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }));
}

RC_GTEST_PROP(ReplogCodec, RandomBatchesRoundTrip, ()) {
  const auto ops = *rc::gen::container<std::vector<ReplOp>>(GenOp());
  const auto ts = *rc::gen::arbitrary<int64_t>();
  const auto before = *rc::gen::inRange<uint64_t>(0, 1ULL << 40);
  const std::string enc = EncodeReplogBatch(ops, ts, before);
  auto dec = DecodeReplogBatch(enc);
  RC_ASSERT(dec.ok());
  RC_ASSERT(dec->ops == ops);
  RC_ASSERT(dec->timestamp_ms == ts);
  RC_ASSERT(dec->cum_bytes == before + enc.size());
  RC_ASSERT(EncodeReplogBatch(dec->ops, dec->timestamp_ms, dec->cum_bytes - enc.size()) == enc);
}

}  // namespace
}  // namespace tideskv::encoding
