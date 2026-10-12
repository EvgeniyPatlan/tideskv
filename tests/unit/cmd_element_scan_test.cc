#include <gtest/gtest.h>

#include <set>

#include "support/command_harness.h"
#include "support/scan_reply.h"
#include "types/cursor_store.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::CommandHarness;
using test::Err;
using test::Int;
using test::kOk;
using test::ParseScan;
using test::ScanReply;

const std::string kEmptyScan = "*2\r\n$1\r\n0\r\n*0\r\n";

struct ElementScan : ::testing::Test {
  // Iterates `cmd key` to the end; returns every element returned (with repeats).
  std::multiset<std::string> Full(const std::string& cmd, const std::string& key,
                                  std::vector<std::string> extra) {
    std::multiset<std::string> seen;
    std::string cursor = "0";
    int calls = 0;
    do {
      ArgList argv = {cmd, key, cursor};
      argv.insert(argv.end(), extra.begin(), extra.end());
      ScanReply r = ParseScan(h.Run(argv));
      seen.insert(r.keys.begin(), r.keys.end());
      cursor = r.cursor;
      EXPECT_LT(++calls, 1000) << cmd << " never finished";
    } while (cursor != "0" && calls < 1000);
    return seen;
  }
  CommandHarness h;
};

TEST_F(ElementScan, HscanReturnsEveryFieldAndValueOnce) {
  EXPECT_EQ(h.Run({"HSET", "h", "a", "1", "b", "2", "c", "3", "d", "4", "e", "5", "f", "6", "g", "7"}),
            Int(7));
  const auto seen = Full("HSCAN", "h", {"COUNT", "2"});
  EXPECT_EQ(seen, (std::multiset<std::string>{"a", "1", "b", "2", "c", "3", "d", "4", "e", "5", "f", "6", "g",
                                              "7"}));
}

TEST_F(ElementScan, HscanMatchFiltersFieldsNotValues) {
  EXPECT_EQ(h.Run({"HSET", "h", "foo", "x", "bar", "foo", "fab", "y"}), Int(3));
  EXPECT_EQ(Full("HSCAN", "h", {"MATCH", "f*", "COUNT", "1"}),
            (std::multiset<std::string>{"fab", "y", "foo", "x"}));
}

TEST_F(ElementScan, HscanErrorOrderFollowsRedis) {
  EXPECT_EQ(h.Run({"HSCAN", "nokey", "0"}), kEmptyScan);
  EXPECT_EQ(h.Run({"HSCAN", "nokey", "0", "BOGUS", "1"}), kEmptyScan);  // lookup before options
  EXPECT_EQ(h.Run({"HSCAN", "nokey", "x"}), Err(kErrInvalidCursor));    // cursor before lookup
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v"}), Int(1));
  EXPECT_EQ(h.Run({"HSCAN", "h", "0", "BOGUS", "1"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"HSCAN", "h", "0", "TYPE", "hash"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"HSCAN", "h", "0", "COUNT", "0"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"HSCAN", "h", "0", "COUNT"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"HSCAN", "s", "0"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"HSCAN", "s", "x"}), Err(kErrInvalidCursor));
  EXPECT_EQ(h.Run({"HSCAN", "s", "0", "BOGUS", "1"}), Err(kErrWrongType));  // type check before options
  EXPECT_EQ(h.Run({"HSCAN", "h", "0", "COUNT", "x"}), Err(kErrNotInteger));
}

TEST_F(ElementScan, ScanRestartsOnAnHscanCursor) {
  EXPECT_EQ(h.Run({"HSET", "h", "a", "1", "b", "2"}), Int(2));
  EXPECT_EQ(h.Run({"MSET", "k1", "v", "k2", "v"}), kOk);
  const ScanReply first = ParseScan(h.Run({"SCAN", "0", "COUNT", "1"}));
  const ScanReply element = ParseScan(h.Run({"HSCAN", "h", "0", "COUNT", "1"}));
  ASSERT_NE(element.cursor, "0");
  const ScanReply r = ParseScan(h.Run({"SCAN", element.cursor, "COUNT", "1"}));
  EXPECT_EQ(r.keys, first.keys);  // restarted, as from cursor 0
}

TEST_F(ElementScan, ForeignAndStaleCursorsRestart) {
  EXPECT_EQ(h.Run({"HSET", "h", "a", "1", "b", "2", "c", "3"}), Int(3));
  EXPECT_EQ(h.Run({"MSET", "k1", "v", "k2", "v"}), kOk);
  const ScanReply keys = ParseScan(h.Run({"SCAN", "0", "COUNT", "1"}));
  ASSERT_NE(keys.cursor, "0");
  ScanReply r = ParseScan(h.Run({"HSCAN", "h", keys.cursor, "COUNT", "1"}));  // a SCAN cursor
  EXPECT_EQ(r.keys, (std::vector<std::string>{"a", "1"}));                    // restarted
  const std::string mid = r.cursor;
  EXPECT_EQ(h.Run({"DEL", "h"}), Int(1));
  EXPECT_EQ(h.Run({"HSET", "h", "z", "9", "y", "8"}), Int(2));  // new oid
  r = ParseScan(h.Run({"HSCAN", "h", mid, "COUNT", "1"}));
  EXPECT_EQ(r.keys, (std::vector<std::string>{"y", "8"}));  // restarted inside the new hash
}

TEST_F(ElementScan, HugeFieldsTerminate) {
  const std::string big(CursorStore::kMaxPositionBytes + 10, 'b');
  const std::string big2 = big + "2";
  EXPECT_EQ(h.Run({"HSET", "h", "a", "1", big, "2", big2, "3", "c", "4"}), Int(4));
  for (const char* count : {"1", "2"}) {
    const auto seen = Full("HSCAN", "h", {"COUNT", count});
    EXPECT_EQ(seen.count("a"), 1u);
    EXPECT_EQ(seen.count("c"), 1u);
    EXPECT_GE(seen.count(big), 1u);
    EXPECT_GE(seen.count(big2), 1u);
  }
}

TEST_F(ElementScan, ScanHandlesBinaryAndHashtagKeys) {  // M2 Task 14 carry-over
  const std::string bin("b\0\r\n\xff", 5);
  EXPECT_EQ(h.Run({"MSET", bin, "1", "{t}a", "2", "{t}b", "3", "", "4"}), kOk);
  std::multiset<std::string> seen;
  std::string cursor = "0";
  int calls = 0;
  do {
    ScanReply r = ParseScan(h.Run({"SCAN", cursor, "COUNT", "1"}));
    seen.insert(r.keys.begin(), r.keys.end());
    cursor = r.cursor;
  } while (cursor != "0" && ++calls < 100);
  EXPECT_LT(calls, 100) << "SCAN never finished";
  EXPECT_EQ(seen, (std::multiset<std::string>{bin, "{t}a", "{t}b", ""}));
}

TEST_F(ElementScan, SscanReturnsMembersOnly) {
  EXPECT_EQ(h.Run({"SADD", "s", "a", "b", "c", "d", "e"}), Int(5));
  EXPECT_EQ(Full("SSCAN", "s", {"COUNT", "2"}), (std::multiset<std::string>{"a", "b", "c", "d", "e"}));
  EXPECT_EQ(Full("SSCAN", "s", {"MATCH", "[ac]"}), (std::multiset<std::string>{"a", "c"}));
  EXPECT_EQ(h.Run({"SSCAN", "nokey", "0"}), kEmptyScan);
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v"}), Int(1));
  EXPECT_EQ(h.Run({"SSCAN", "h", "0"}), Err(kErrWrongType));
}

}  // namespace
}  // namespace tideskv
