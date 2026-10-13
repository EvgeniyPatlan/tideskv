#include "resp/reply_parser.h"

#include <gtest/gtest.h>

#include <cmath>

#include "resp/reply_builder.h"

namespace tideskv::resp {
namespace {

ReplyNode Parse(std::string_view s) {
  auto r = ParseReply(s);
  EXPECT_TRUE(r.ok()) << r.status().ToString() << " for " << s;
  return r.ok() ? *r : ReplyNode{};
}

TEST(ReplyParser, Scalars) {
  EXPECT_EQ(Parse("+OK\r\n").type, ReplyNode::Type::kStatus);
  EXPECT_EQ(Parse("+OK\r\n").str, "OK");
  EXPECT_EQ(Parse("-ERR boom\r\n").str, "ERR boom");
  EXPECT_EQ(Parse(":-42\r\n").integer, -42);
  EXPECT_EQ(Parse("$3\r\na\r\n\r\n").str, std::string("a\r\n"));
  EXPECT_EQ(Parse("$-1\r\n").type, ReplyNode::Type::kNullBulk);
  EXPECT_EQ(Parse("*-1\r\n").type, ReplyNode::Type::kNullArray);
  EXPECT_EQ(Parse("_\r\n").type, ReplyNode::Type::kNull);
  EXPECT_TRUE(Parse("#t\r\n").boolean);
  EXPECT_FALSE(Parse("#f\r\n").boolean);
  EXPECT_DOUBLE_EQ(Parse(",3.5\r\n").dbl, 3.5);
  EXPECT_TRUE(std::isinf(Parse(",-inf\r\n").dbl));
  EXPECT_EQ(Parse("(12345678901234567890\r\n").str, "12345678901234567890");
  const ReplyNode v = Parse("=8\r\ntxt:abcd\r\n");
  EXPECT_EQ(v.type, ReplyNode::Type::kVerbatim);
  EXPECT_EQ(v.format, "txt");
  EXPECT_EQ(v.str, "abcd");
}

TEST(ReplyParser, Aggregates) {
  const ReplyNode a = Parse("*2\r\n$1\r\na\r\n*1\r\n:1\r\n");
  ASSERT_EQ(a.elems.size(), 2u);
  EXPECT_EQ(a.elems[1].elems[0].integer, 1);
  const ReplyNode m = Parse("%1\r\n+k\r\n:1\r\n");
  EXPECT_EQ(m.type, ReplyNode::Type::kMap);
  ASSERT_EQ(m.elems.size(), 2u);  // k, v
  EXPECT_EQ(Parse("~2\r\n:1\r\n:2\r\n").type, ReplyNode::Type::kSet);
  EXPECT_EQ(Parse("|1\r\n+a\r\n+b\r\n+real\r\n").str, "real");  // attributes are skipped
}

TEST(ReplyParser, RoundTripsEverythingReplyBuilderWrites) {
  for (int proto : {2, 3}) {
    ReplyBuilder b(proto);
    b.ArrayHeader(4);
    b.Double(1.5);
    b.MapHeader(1);
    b.Bulk("k");
    b.Boolean(true);
    b.BigNumber("123");
    b.Verbatim("hi", "markdown");
    auto r = ParseReply(b.buffer());
    ASSERT_TRUE(r.ok()) << proto;
    EXPECT_EQ(r->elems.size(), 4u);
  }
}

TEST(ReplyParser, RejectsIncompleteOrTrailingInput) {
  EXPECT_FALSE(ParseReply("$3\r\nab").ok());
  EXPECT_FALSE(ParseReply("+OK\r\n+OK\r\n").ok());
  EXPECT_FALSE(ParseReply("?\r\n").ok());
}

}  // namespace
}  // namespace tideskv::resp
