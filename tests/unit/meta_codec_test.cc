#include "encoding/meta_codec.h"

#include <gtest/gtest.h>

namespace tideskv::encoding {
namespace {

TEST(MetaCodec, StringRoundTripIsBinarySafe) {
  Metadata m;
  m.type = ValueType::kString;
  m.expire_ms = 1700000000123;
  const std::string payload("v\0\r\nx", 5);
  auto decoded = DecodeMetadata(EncodeMetadata(m, payload));
  ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
  EXPECT_EQ(decoded->meta.type, ValueType::kString);
  EXPECT_EQ(decoded->meta.expire_ms, 1700000000123);
  EXPECT_EQ(decoded->payload, payload);
}

TEST(MetaCodec, HashRoundTrip) {
  Metadata m;
  m.type = ValueType::kHash;
  m.oid = 42;
  m.size = 3;
  const std::string enc = EncodeMetadata(m);
  EXPECT_EQ(enc.size(), 26u);
  auto d = DecodeMetadata(enc);
  ASSERT_TRUE(d.ok());
  EXPECT_EQ(d->meta.oid, 42u);
  EXPECT_EQ(d->meta.size, 3u);
  EXPECT_EQ(d->meta.expire_ms, 0);
  EXPECT_TRUE(d->meta.IsCollection());
}

TEST(MetaCodec, ListRoundTripKeepsHeadAndTail) {
  Metadata m;
  m.type = ValueType::kList;
  m.oid = 9;
  m.size = 2;
  m.head = kListInitialIndex - 1;
  m.tail = kListInitialIndex;
  const std::string enc = EncodeMetadata(m);
  EXPECT_EQ(enc.size(), 42u);
  auto d = DecodeMetadata(enc);
  ASSERT_TRUE(d.ok());
  EXPECT_EQ(d->meta.head, kListInitialIndex - 1);
  EXPECT_EQ(d->meta.tail, kListInitialIndex);
}

TEST(MetaCodec, EmptyListDefaults) {
  Metadata m;
  EXPECT_LT(m.tail, m.head);  // tail < head means empty
}

TEST(MetaCodec, RejectsCorruptInput) {
  EXPECT_EQ(DecodeMetadata("short").status().code(), StatusCode::kCorruption);
  std::string bad_type = EncodeMetadata(Metadata{});
  bad_type[0] = 9;
  EXPECT_EQ(DecodeMetadata(bad_type).status().code(), StatusCode::kCorruption);
  Metadata h;
  h.type = ValueType::kSet;
  EXPECT_EQ(DecodeMetadata(EncodeMetadata(h) + "x").status().code(), StatusCode::kCorruption);
  EXPECT_EQ(DecodeMetadata(EncodeMetadata(h).substr(0, 20)).status().code(), StatusCode::kCorruption);
}

TEST(MetaCodec, Expiry) {
  Metadata m;
  EXPECT_FALSE(m.IsExpired(1));  // 0 = no expiry
  m.expire_ms = 1000;
  EXPECT_FALSE(m.IsExpired(999));
  EXPECT_TRUE(m.IsExpired(1000));
}

TEST(MetaCodec, TypeNames) {
  EXPECT_EQ(TypeName(ValueType::kString), "string");
  EXPECT_EQ(TypeName(ValueType::kHash), "hash");
  EXPECT_EQ(TypeName(ValueType::kList), "list");
  EXPECT_EQ(TypeName(ValueType::kSet), "set");
  EXPECT_EQ(TypeName(ValueType::kZSet), "zset");
}

}  // namespace
}  // namespace tideskv::encoding
