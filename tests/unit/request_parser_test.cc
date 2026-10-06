#include "resp/request_parser.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <algorithm>

namespace tideskv::resp {
namespace {

using Commands = std::vector<std::vector<std::string>>;

// Feeds chunks exactly like a connection does: keep unconsumed bytes, append new ones.
Commands ParseAll(const std::vector<std::string>& chunks, std::string* error = nullptr,
                  ParserLimits limits = {}) {
  RequestParser parser(limits);
  std::string pending;
  Commands out;
  for (const std::string& chunk : chunks) {
    pending += chunk;
    for (;;) {
      size_t consumed = 0;
      const ParseStatus st = parser.Parse(pending, &consumed);
      pending.erase(0, consumed);
      if (st == ParseStatus::kCommand) {
        out.push_back(parser.args());
        continue;
      }
      if (st == ParseStatus::kError && error != nullptr) *error = parser.error();
      if (st == ParseStatus::kError) return out;
      break;
    }
  }
  return out;
}

std::string Encode(const std::vector<std::string>& argv) {
  std::string out = "*" + std::to_string(argv.size()) + "\r\n";
  for (const std::string& a : argv) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  return out;
}

std::string ErrorFor(const std::string& input, ParserLimits limits = {}) {
  std::string error;
  ParseAll({input}, &error, limits);
  return error;
}

TEST(RequestParser, SingleCommand) {
  EXPECT_EQ(ParseAll({Encode({"SET", "k", "v"})}), (Commands{{"SET", "k", "v"}}));
}

TEST(RequestParser, PipelinedCommandsInOneChunk) {
  EXPECT_EQ(ParseAll({Encode({"PING"}) + Encode({"ECHO", "hi"})}), (Commands{{"PING"}, {"ECHO", "hi"}}));
}

TEST(RequestParser, ByteByByteEqualsWhole) {  // Review Focus #1
  const std::string wire = Encode({"SET", "key", std::string("v\r\n\0x", 5)}) + "PING\r\n";
  std::vector<std::string> bytes;
  for (char c : wire) bytes.emplace_back(1, c);
  EXPECT_EQ(ParseAll(bytes), ParseAll({wire}));
  EXPECT_EQ(ParseAll(bytes).size(), 2u);
}

TEST(RequestParser, BinarySafeAndEmptyBulk) {
  const std::string bin("a\r\n\0b", 5);
  EXPECT_EQ(ParseAll({Encode({"ECHO", bin, ""})}), (Commands{{"ECHO", bin, ""}}));
}

TEST(RequestParser, InlineCommands) {
  EXPECT_EQ(ParseAll({"PING\r\n"}), (Commands{{"PING"}}));
  EXPECT_EQ(ParseAll({"set k v\n"}), (Commands{{"set", "k", "v"}}));
  EXPECT_EQ(ParseAll({"  echo   \"hello world\"  \r\n"}), (Commands{{"echo", "hello world"}}));
  EXPECT_EQ(ParseAll({"echo \"a\\\"b\\n\"\r\n"}), (Commands{{"echo", "a\"b\n"}}));
  EXPECT_EQ(ParseAll({"echo 'x y'\r\n"}), (Commands{{"echo", "x y"}}));
}

TEST(RequestParser, BlankLinesAndEmptyArraysAreIgnored) {
  EXPECT_EQ(ParseAll({"\r\n\r\nPING\r\n"}), (Commands{{"PING"}}));
  EXPECT_EQ(ParseAll({"*0\r\n*-1\r\n" + Encode({"PING"})}), (Commands{{"PING"}}));
}

TEST(RequestParser, ProtocolErrors) {
  EXPECT_EQ(ErrorFor("*abc\r\n"), "Protocol error: invalid multibulk length");
  EXPECT_EQ(ErrorFor("*1\r\n$-5\r\n"), "Protocol error: invalid bulk length");
  EXPECT_EQ(ErrorFor("*1\r\n$abc\r\n"), "Protocol error: invalid bulk length");
  EXPECT_EQ(ErrorFor("*1\r\n:5\r\n"), "Protocol error: expected '$', got ':'");
  EXPECT_EQ(ErrorFor("*1\r\n$3\r\nabcXY"), "Protocol error: expected CRLF after bulk string");
  EXPECT_EQ(ErrorFor("echo \"unbalanced\r\n"), "Protocol error: unbalanced quotes in request");
}

TEST(RequestParser, HostileLengthsRejectedWithoutAllocating) {  // Review Focus #2
  ParserLimits small;
  small.max_bulk_len = 16;
  small.max_multibulk_len = 4;
  EXPECT_EQ(ErrorFor("*1\r\n$1000000000000\r\n", small), "Protocol error: invalid bulk length");
  EXPECT_EQ(ErrorFor("*1\r\n$17\r\n", small), "Protocol error: invalid bulk length");
  EXPECT_EQ(ErrorFor("*5\r\n", small), "Protocol error: invalid multibulk length");
  EXPECT_EQ(ErrorFor("*99999999999999999999\r\n"), "Protocol error: invalid multibulk length");
  EXPECT_EQ(ErrorFor("*1\r\n$999999999999\r\n"), "Protocol error: invalid bulk length");  // > 512MB default
}

TEST(RequestParser, OverlongLinesRejected) {
  ParserLimits small;
  small.max_inline_len = 8;
  EXPECT_EQ(ErrorFor("aaaaaaaaaaaaaaaaaaaa", small), "Protocol error: too big inline request");
  EXPECT_EQ(ErrorFor("*111111111111111111111", small), "Protocol error: too big mbulk count string");
  EXPECT_EQ(ErrorFor("*1\r\n$111111111111111111", small), "Protocol error: too big bulk count string");
}

TEST(RequestParser, ErrorIsSticky) {
  RequestParser p;
  size_t consumed = 0;
  ASSERT_EQ(p.Parse("*x\r\n", &consumed), ParseStatus::kError);
  EXPECT_EQ(p.Parse(Encode({"PING"}), &consumed), ParseStatus::kError);
}

TEST(RequestParser, BufferedBytesTracksPartialBulk) {
  RequestParser p;
  size_t consumed = 0;
  ASSERT_EQ(p.Parse("*1\r\n$10\r\nabcd", &consumed), ParseStatus::kNeedMore);
  EXPECT_EQ(consumed, 13u);
  EXPECT_EQ(p.buffered_bytes(), 4u);
}

RC_GTEST_PROP(RequestParserProp, ArbitrarySplitsYieldSameCommand, (std::vector<std::string> argv)) {
  RC_PRE(!argv.empty());
  const std::string wire = Encode(argv);
  auto cuts = *rc::gen::container<std::vector<size_t>>(rc::gen::inRange<size_t>(0, wire.size() + 1));
  std::sort(cuts.begin(), cuts.end());
  std::vector<std::string> chunks;
  size_t prev = 0;
  for (size_t c : cuts) {
    chunks.push_back(wire.substr(prev, c - prev));
    prev = c;
  }
  chunks.push_back(wire.substr(prev));
  RC_ASSERT(ParseAll(chunks) == Commands{argv});
}

}  // namespace
}  // namespace tideskv::resp
