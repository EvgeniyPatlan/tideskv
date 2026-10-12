#include <gtest/gtest.h>

#include <set>

#include "commands/reply_util.h"
#include "support/command_harness.h"
#include "types/cursor_store.h"

namespace tideskv {
namespace {

using test::CommandHarness;
using test::Err;
using test::kNil;
using test::kOk;

// Parses "*2\r\n$<n>\r\n<cursor>\r\n*<m>\r\n($<k>\r\n<key>\r\n)*" (binary-safe).
struct ScanReply {
  std::string cursor;
  std::vector<std::string> keys;
};

std::string TakeBulk(const std::string& s, size_t* pos) {
  const size_t crlf = s.find("\r\n", *pos);
  const size_t n = std::stoul(s.substr(*pos + 1, crlf - *pos - 1));
  std::string out = s.substr(crlf + 2, n);
  *pos = crlf + 2 + n + 2;
  return out;
}

ScanReply ParseScan(const std::string& raw) {
  ScanReply r;
  size_t pos = raw.find("\r\n") + 2;  // skip *2
  r.cursor = TakeBulk(raw, &pos);
  const size_t crlf = raw.find("\r\n", pos);
  const size_t m = std::stoul(raw.substr(pos + 1, crlf - pos - 1));
  pos = crlf + 2;
  for (size_t i = 0; i < m; ++i) r.keys.push_back(TakeBulk(raw, &pos));
  return r;
}

struct ScanCmds : ::testing::Test {
  CommandHarness h;
  std::string Run(std::initializer_list<const char*> a) { return h.Run(ArgList(a.begin(), a.end())); }
  std::multiset<std::string> FullScan(std::vector<std::string> extra = {}) {
    std::multiset<std::string> seen;
    std::string cursor = "0";
    int calls = 0;
    do {
      ArgList argv = {"SCAN", cursor};
      argv.insert(argv.end(), extra.begin(), extra.end());
      ScanReply r = ParseScan(h.Run(argv));
      seen.insert(r.keys.begin(), r.keys.end());
      cursor = r.cursor;
      EXPECT_LT(++calls, 10000) << "SCAN never finished";
    } while (cursor != "0" && calls < 10000);
    return seen;
  }
};

TEST_F(ScanCmds, FullIterationReturnsEveryKeyOnce) {
  for (int i = 0; i < 200; ++i) h.Run({"SET", "k" + std::to_string(i), "v"});
  const auto seen = FullScan({"COUNT", "7"});
  EXPECT_EQ(seen.size(), 200u);
  EXPECT_EQ(std::set<std::string>(seen.begin(), seen.end()).size(), 200u);
}

TEST_F(ScanCmds, KeysPresentThroughoutAreReturnedDespiteConcurrentWrites) {
  for (int i = 0; i < 100; ++i) h.Run({"SET", "stable" + std::to_string(i), "v"});
  std::set<std::string> seen;
  std::string cursor = "0";
  int round = 0;
  do {
    ScanReply r = ParseScan(h.Run({"SCAN", cursor, "COUNT", "5"}));
    seen.insert(r.keys.begin(), r.keys.end());
    cursor = r.cursor;
    h.Run({"SET", "new" + std::to_string(round), "v"});  // inserts during the scan
    h.Run({"DEL", "new" + std::to_string(round - 1)});
    ++round;
  } while (cursor != "0");
  for (int i = 0; i < 100; ++i) EXPECT_EQ(seen.count("stable" + std::to_string(i)), 1u) << i;
}

TEST_F(ScanCmds, MatchAndType) {
  EXPECT_EQ(Run({"MSET", "user:1", "a", "user:2", "b", "other", "c"}), kOk);
  encoding::Metadata m;
  m.type = encoding::ValueType::kHash;
  m.oid = 1;
  h.SeedMeta(0, "user:h", m);
  EXPECT_EQ(FullScan({"MATCH", "user:*"}).size(), 3u);
  EXPECT_EQ(FullScan({"MATCH", "user:*", "TYPE", "string"}).size(), 2u);
  EXPECT_EQ(FullScan({"TYPE", "HASH"}), std::multiset<std::string>{"user:h"});
  EXPECT_TRUE(FullScan({"TYPE", "nosuchtype"}).empty());
  EXPECT_EQ(FullScan({"MATCH", "*"}).size(), 4u);
}

TEST_F(ScanCmds, SkipsExpiredAndStaysInItsDb) {  // Review Focus #1
  EXPECT_EQ(Run({"SET", "live", "v"}), kOk);
  EXPECT_EQ(Run({"SET", "dead", "v", "PX", "1"}), kOk);
  h.clock().Advance(1);
  h.session.db = 2;
  EXPECT_EQ(Run({"SET", "other-db", "v"}), kOk);
  h.session.db = 0;
  EXPECT_EQ(FullScan(), std::multiset<std::string>{"live"});
}

TEST_F(ScanCmds, CursorErrorsAndRestarts) {
  for (int i = 0; i < 30; ++i) h.Run({"SET", "k" + std::to_string(i), "v"});
  EXPECT_EQ(Run({"SCAN", "abc"}), Err(kErrInvalidCursor));
  // Redis 7.2 parseScanCursorOrReply uses strtoul: leading space, trailing junk and ERANGE fail.
  EXPECT_EQ(Run({"SCAN", " 1"}), Err(kErrInvalidCursor));
  EXPECT_EQ(Run({"SCAN", "1x"}), Err(kErrInvalidCursor));
  EXPECT_EQ(Run({"SCAN", "-"}), Err(kErrInvalidCursor));
  EXPECT_EQ(Run({"SCAN", "18446744073709551616"}), Err(kErrInvalidCursor));
  // ...but '-1' wraps, and '+5', '007' and '' parse: all unknown cursors (or 0) that restart.
  for (const char* c : {"-1", "+5", "007", ""}) {
    const std::string raw = Run({"SCAN", c, "COUNT", "100"});
    ASSERT_EQ(raw.substr(0, 2), "*2") << '"' << c << "\" -> " << raw;
    ScanReply r = ParseScan(raw);
    EXPECT_EQ(r.keys.size(), 30u) << '"' << c << '"';
    EXPECT_EQ(r.cursor, "0") << '"' << c << '"';
  }
  EXPECT_EQ(Run({"SCAN", "0", "COUNT", "0"}), Err(kErrSyntax));
  EXPECT_EQ(Run({"SCAN", "0", "COUNT", "x"}), Err(kErrNotInteger));
  EXPECT_EQ(Run({"SCAN", "0", "MATCH"}), Err(kErrSyntax));
  EXPECT_EQ(Run({"SCAN", "0", "BOGUS", "1"}), Err(kErrSyntax));
  ScanReply unknown = ParseScan(Run({"SCAN", "987654321", "COUNT", "100"}));  // unknown cursor: restart
  EXPECT_EQ(unknown.keys.size(), 30u);
  EXPECT_EQ(unknown.cursor, "0");
  // A cursor saved in db 1 sorts after all of db 0; used in db 0 it must restart (D9), not
  // resume past every db-0 key.
  h.session.db = 1;
  for (int i = 0; i < 20; ++i) h.Run({"SET", "db1-" + std::to_string(i), "v"});
  ScanReply page = ParseScan(Run({"SCAN", "0", "COUNT", "5"}));
  ASSERT_NE(page.cursor, "0");
  h.session.db = 0;
  ScanReply restarted = ParseScan(h.Run({"SCAN", page.cursor, "COUNT", "100"}));
  EXPECT_EQ(restarted.keys.size(), 30u);
  EXPECT_EQ(restarted.cursor, "0");
}

// Keys whose meta key exceeds CursorStore::kMaxPositionBytes are saved as a truncated
// position. All of these share the hashtag {h}, so they sit in one slot ordered by bytes:
// "{h}a" < every huge key < "{h}z". Huge keys share their first 1 MiB + 3 bytes, so their meta
// keys share the whole saved prefix and differ only in `tail`.
std::string HugeKey(const std::string& tail) {
  return "{h}" + std::string(CursorStore::kMaxPositionBytes, 'x') + tail;
}

struct HugeKeyScan : ScanCmds {
  static constexpr int kMaxCalls = 100;  // the livelock bug fails here instead of hanging

  void SetKeys(const std::vector<std::string>& keys) {
    for (const std::string& k : keys) ASSERT_EQ(h.Run({"SET", k, "v"}), kOk);
  }
  // Runs one SCAN page; returns false once the cursor is back to 0.
  bool Step(std::string* cursor, const std::string& count, std::multiset<std::string>* seen) {
    ScanReply r = ParseScan(h.Run({"SCAN", *cursor, "COUNT", count}));
    seen->insert(r.keys.begin(), r.keys.end());
    *cursor = r.cursor;
    return *cursor != "0";
  }
  // Full scan; *finished is false if the cursor did not return to 0 within kMaxCalls.
  std::multiset<std::string> ScanAll(const std::string& count, bool* finished) {
    std::multiset<std::string> seen;
    std::string cursor = "0";
    for (int calls = 0; calls < kMaxCalls; ++calls) {
      if (!Step(&cursor, count, &seen)) {
        *finished = true;
        return seen;
      }
    }
    *finished = false;
    return seen;
  }
  // EXPECT_TRUE rather than EXPECT_EQ: no multi-MiB failure dumps.
  static void ExpectAllSeen(const std::multiset<std::string>& seen, const std::vector<std::string>& keys) {
    for (size_t i = 0; i < keys.size(); ++i) EXPECT_TRUE(seen.count(keys[i]) >= 1) << "key #" << i;
  }
};

TEST_F(HugeKeyScan, OneHugeKeyTerminatesForSmallCounts) {
  const std::vector<std::string> keys = {"{h}a", HugeKey(""), "{h}z", "s1", "s2", "s3"};
  SetKeys(keys);
  for (const char* count : {"1", "2"}) {
    bool finished = false;
    const auto seen = ScanAll(count, &finished);
    EXPECT_TRUE(finished) << "COUNT " << count << ": SCAN never finished";
    ExpectAllSeen(seen, keys);
  }
}

TEST_F(HugeKeyScan, HugeKeysSharingTheSavedPrefixEachReturned) {
  const std::vector<std::string> keys = {"{h}a", HugeKey("1"), HugeKey("2"), HugeKey("3"), "{h}z", "s1"};
  SetKeys(keys);
  for (const char* count : {"1", "2", "3"}) {
    bool finished = false;
    const auto seen = ScanAll(count, &finished);
    EXPECT_TRUE(finished) << "COUNT " << count << ": SCAN never finished";
    ExpectAllSeen(seen, keys);
  }
}

TEST_F(HugeKeyScan, HugeKeyDeletedBetweenCallsStillTerminates) {
  for (const std::vector<std::string>& keys :
       {std::vector<std::string>{"{h}a", HugeKey("1"), "{h}z", "s1"},
        std::vector<std::string>{"{h}a", HugeKey("1"), HugeKey("2"), "{h}z", "s1"}}) {
    ASSERT_EQ(h.Run({"FLUSHALL"}), kOk);
    SetKeys(keys);
    std::multiset<std::string> seen;
    std::string cursor = "0";
    int calls = 0;
    bool more = true;
    // Advance until the cursor sits on HugeKey("1"), then delete it.
    while (more && seen.count(HugeKey("1")) == 0 && ++calls < kMaxCalls) more = Step(&cursor, "1", &seen);
    ASSERT_TRUE(more) << "scan finished before the huge key was the cursor";
    ASSERT_EQ(h.Run({"DEL", HugeKey("1")}), test::Int(1));
    while (more && ++calls < kMaxCalls) more = Step(&cursor, "1", &seen);
    EXPECT_FALSE(more) << "SCAN never finished after deleting the huge key";
    ExpectAllSeen(seen, keys);  // the deleted key was returned before its deletion
  }
}

TEST_F(HugeKeyScan, LaterHugeKeyDeletedAfterItsPageStillTerminates) {
  const std::vector<std::string> keys = {"{h}a", HugeKey("1"), HugeKey("2"), "{h}z", "s1"};
  SetKeys(keys);
  std::multiset<std::string> seen;
  std::string cursor = "0";
  int calls = 0;
  bool more = true;
  // COUNT 1: the page that returns HugeKey("2") ends on it, so the cursor sits on it.
  while (more && seen.count(HugeKey("2")) == 0 && ++calls < kMaxCalls) more = Step(&cursor, "1", &seen);
  ASSERT_TRUE(more) << "scan finished before the later huge key was the cursor";
  ASSERT_EQ(h.Run({"DEL", HugeKey("2")}), test::Int(1));
  while (more && ++calls < kMaxCalls) more = Step(&cursor, "1", &seen);
  EXPECT_FALSE(more) << "SCAN never finished after deleting the later huge key";
  ExpectAllSeen(seen, keys);
}

// Meta key = [db][slot u16][user key], so its size is user key size + 3.
std::string KeyWithMetaSize(size_t meta_size, char fill) { return "{h}" + std::string(meta_size - 6, fill); }

TEST_F(HugeKeyScan, MetaKeysAtAndJustPastTheSavedLimit) {
  const std::vector<std::string> keys = {"{h}a",
                                         KeyWithMetaSize(CursorStore::kMaxPositionBytes, 'x'),
                                         KeyWithMetaSize(CursorStore::kMaxPositionBytes + 1, 'x'),
                                         KeyWithMetaSize(CursorStore::kMaxPositionBytes + 1, 'y'),
                                         "{h}z",
                                         "s1"};
  SetKeys(keys);
  bool finished = false;
  const auto seen = ScanAll("1", &finished);
  EXPECT_TRUE(finished) << "SCAN never finished";
  ExpectAllSeen(seen, keys);
}

// Redis 7.2 only treats exactly "*" as "no filter": an empty pattern matches only the empty key.
TEST_F(ScanCmds, EmptyPatternMatchesOnlyTheEmptyKey) {
  EXPECT_EQ(Run({"MSET", "", "x", "a", "y"}), kOk);
  EXPECT_EQ(Run({"KEYS", ""}), test::Arr({test::Bulk("")}));
  EXPECT_EQ(FullScan({"MATCH", ""}), std::multiset<std::string>{""});
  EXPECT_EQ(Run({"DEL", ""}), test::Int(1));
  EXPECT_EQ(Run({"KEYS", ""}), "*0\r\n");
  EXPECT_TRUE(FullScan({"MATCH", ""}).empty());
}

TEST_F(ScanCmds, Keys) {
  const std::string bin("b\0in", 4);
  EXPECT_EQ(Run({"MSET", "foo", "1", "foobar", "2", "bar", "3"}), kOk);
  EXPECT_EQ(h.Run({"SET", bin, "4"}), kOk);
  const std::string r = Run({"KEYS", "foo*"});
  EXPECT_EQ(r.substr(0, 4), "*2\r\n");
  EXPECT_NE(r.find("$3\r\nfoo\r\n"), std::string::npos);
  EXPECT_NE(r.find("$6\r\nfoobar\r\n"), std::string::npos);
  EXPECT_EQ(Run({"KEYS", "*"}).substr(0, 4), "*4\r\n");
  EXPECT_EQ(Run({"KEYS", "nomatch*"}), "*0\r\n");
}

TEST_F(ScanCmds, RandomKey) {
  EXPECT_EQ(Run({"RANDOMKEY"}), kNil);
  EXPECT_EQ(Run({"SET", "only", "v"}), kOk);
  for (int i = 0; i < 20; ++i) EXPECT_EQ(Run({"RANDOMKEY"}), test::Bulk("only"));
  EXPECT_EQ(Run({"SET", "gone", "v", "PX", "1"}), kOk);
  EXPECT_EQ(Run({"DEL", "only"}), test::Int(1));
  h.clock().Advance(1);
  EXPECT_EQ(Run({"RANDOMKEY"}), kNil);  // only an expired key remains
}

}  // namespace
}  // namespace tideskv
