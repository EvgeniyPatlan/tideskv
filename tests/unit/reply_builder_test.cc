#include "resp/reply_builder.h"

#include <gtest/gtest.h>

#include <limits>

namespace tideskv::resp {
namespace {

std::string Build2(void (*fn)(ReplyBuilder&)) {
  ReplyBuilder r(2);
  fn(r);
  return r.Take();
}
std::string Build3(void (*fn)(ReplyBuilder&)) {
  ReplyBuilder r(3);
  fn(r);
  return r.Take();
}

TEST(ReplyBuilder, ScalarsSameInBothProtocols) {
  auto simple = [](ReplyBuilder& r) { r.SimpleString("OK"); };
  auto error = [](ReplyBuilder& r) { r.Error("ERR syntax error"); };
  auto integer = [](ReplyBuilder& r) { r.Integer(-42); };
  auto bulk = [](ReplyBuilder& r) { r.Bulk(std::string_view("a\0b", 3)); };
  for (auto build : {Build2, Build3}) {
    EXPECT_EQ(build(simple), "+OK\r\n");
    EXPECT_EQ(build(error), "-ERR syntax error\r\n");
    EXPECT_EQ(build(integer), ":-42\r\n");
    EXPECT_EQ(build(bulk), std::string("$3\r\na\0b\r\n", 9));
  }
}

TEST(ReplyBuilder, SanitizesLineBreaksInSimpleAndError) {
  ReplyBuilder r;
  r.Error("ERR bad\r\ninput");
  r.SimpleString("a\nb");
  EXPECT_EQ(r.Take(), "-ERR bad  input\r\n+a b\r\n");
}

TEST(ReplyBuilder, NullsDifferByProtocol) {
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.Null(); }), "$-1\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.Null(); }), "_\r\n");
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.NullArray(); }), "*-1\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.NullArray(); }), "_\r\n");
}

TEST(ReplyBuilder, AggregateHeaders) {
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.ArrayHeader(3); }), "*3\r\n");
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.MapHeader(2); }), "*4\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.MapHeader(2); }), "%2\r\n");
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.SetHeader(2); }), "*2\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.SetHeader(2); }), "~2\r\n");
}

TEST(ReplyBuilder, DoubleAndBoolean) {
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.Double(1.5); }), "$3\r\n1.5\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.Double(1.5); }), ",1.5\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.Double(0.1); }), ",0.1\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.Double(std::numeric_limits<double>::infinity()); }), ",inf\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.Double(-std::numeric_limits<double>::infinity()); }), ",-inf\r\n");
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.Boolean(true); }), ":1\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.Boolean(false); }), "#f\r\n");
}

TEST(ReplyBuilder, Verbatim) {
  EXPECT_EQ(Build2([](ReplyBuilder& r) { r.Verbatim("hi"); }), "$2\r\nhi\r\n");
  EXPECT_EQ(Build3([](ReplyBuilder& r) { r.Verbatim("hi"); }), "=6\r\ntxt:hi\r\n");
}

TEST(ReplyBuilder, TruncateDropsTrailingBytes) {
  ReplyBuilder r;
  r.SimpleString("OK");
  const size_t mark = r.size();
  r.ArrayHeader(2);
  r.Truncate(mark);
  EXPECT_EQ(r.buffer(), "+OK\r\n");
  r.Truncate(mark + 100);  // larger than the buffer: no-op
  EXPECT_EQ(r.buffer(), "+OK\r\n");
  r.Truncate(0);
  EXPECT_TRUE(r.empty());
}

TEST(ReplyBuilder, ProtocolSwitchAndTake) {
  ReplyBuilder r(2);
  r.SetProtocol(3);
  EXPECT_EQ(r.protocol(), 3);
  r.Null();
  EXPECT_EQ(r.size(), 3u);
  EXPECT_EQ(r.Take(), "_\r\n");
  EXPECT_TRUE(r.empty());
}

TEST(ReplyBuilder, RawAppendsEncodedBytes) {
  resp::ReplyBuilder r;
  r.Integer(1);
  r.Raw("*-1\r\n");
  EXPECT_EQ(r.Take(), ":1\r\n*-1\r\n");
}

TEST(ReplyBuilder, DoubleUsesRedisFormatting) {
  ReplyBuilder r2(2), r3(3);
  r2.Double(1e20);
  r3.Double(1.5);
  r3.Double(-std::numeric_limits<double>::infinity());
  EXPECT_EQ(r2.Take(), "$5\r\n1e+20\r\n");
  EXPECT_EQ(r3.Take(), ",1.5\r\n,-inf\r\n");
}

}  // namespace
}  // namespace tideskv::resp
