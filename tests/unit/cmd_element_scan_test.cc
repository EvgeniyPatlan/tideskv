#include <gtest/gtest.h>

#include <map>
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

TEST(ZScan, SmallZsetIsOneReplyInScoreOrder) {
  test::CommandHarness h;
  EXPECT_EQ(h.Run({"ZADD", "z", "2", "b", "1", "a", "0.1", "c"}), test::Int(3));
  EXPECT_EQ(h.Run({"ZSCAN", "z", "0", "COUNT", "1"}),
            test::Arr({test::Bulk("0"), test::Arr({test::Bulk("c"), test::Bulk("0.1"), test::Bulk("a"),
                                                   test::Bulk("1"), test::Bulk("b"), test::Bulk("2")})}));
  EXPECT_EQ(h.Run({"ZSCAN", "z", "0", "MATCH", "a*"}),
            test::Arr({test::Bulk("0"), test::Arr({test::Bulk("a"), test::Bulk("1")})}));
  EXPECT_EQ(h.Run({"ZSCAN", "nokey", "0", "BOGUS"}),
            test::Arr({test::Bulk("0"), "*0\r\n"}));  // options unchecked
  EXPECT_EQ(h.Run({"ZSCAN", "z", "0", "BOGUS"}), test::Err(kErrSyntax));
}

TEST(ZScan, LargeZsetPagesWithLongDoubleText) {
  test::CommandHarness h;
  for (int i = 0; i < 130; ++i) {
    ASSERT_EQ(h.Run({"ZADD", "z", "0.1", "m" + std::to_string(1000 + i)}), test::Int(1));
  }
  std::set<std::string> seen;
  std::string cursor = "0";
  do {
    const test::ScanReply page = test::ParseScan(h.Run({"ZSCAN", "z", cursor, "COUNT", "50"}));
    cursor = page.cursor;
    for (size_t i = 0; i + 1 < page.keys.size(); i += 2) {  // flat [member, score, ...]
      seen.insert(page.keys[i]);
      EXPECT_EQ(page.keys[i + 1], "0.10000000000000001");  // %.17Lg on a large zset
    }
  } while (cursor != "0");
  EXPECT_EQ(seen.size(), 130u);
}

// <= 128 members but one longer than 64 bytes: Redis keeps it as a skiplist, so ZSCAN takes the large path.
TEST(ZScan, LongMemberTakesTheLargePath) {
  test::CommandHarness h;
  const std::string long_member(65, 'x');
  ASSERT_EQ(h.Run({"ZADD", "z", "0.1", "a", "0.1", long_member, "1.5", "b"}), test::Int(3));
  std::map<std::string, std::string> seen;
  std::string cursor = "0";
  do {
    const test::ScanReply page = test::ParseScan(h.Run({"ZSCAN", "z", cursor}));
    cursor = page.cursor;
    for (size_t i = 0; i + 1 < page.keys.size(); i += 2) seen[page.keys[i]] = page.keys[i + 1];
  } while (cursor != "0");
  EXPECT_EQ(seen, (std::map<std::string, std::string>{
                      {"a", "0.10000000000000001"}, {long_member, "0.10000000000000001"}, {"b", "1.5"}}));
}

}  // namespace
}  // namespace tideskv
