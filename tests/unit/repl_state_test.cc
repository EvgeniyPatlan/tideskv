#include "repl/repl_state.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "storage/memory_storage.h"
#include "support/faulty_storage.h"

namespace tideskv::repl {
namespace {

std::string Id(char c) { return std::string(kReplIdLength, c); }

TEST(ReplState, NewReplIdIsFortyLowercaseHexAndRandom) {
  const std::string a = NewReplId(), b = NewReplId();
  EXPECT_TRUE(IsValidReplId(a));
  EXPECT_NE(a, b);
  EXPECT_FALSE(IsValidReplId("ABC"));
  EXPECT_FALSE(IsValidReplId(std::string(40, 'g')));
  EXPECT_FALSE(IsValidReplId(std::string(40, 'A')));
  EXPECT_FALSE(IsValidReplId(std::string(41, 'a')));
}

TEST(ReplState, SysKeyNamesAreStable) {
  EXPECT_EQ(kSysReplId, "repl_id");
  EXPECT_EQ(kSysReplHistory, "repl_history");
  EXPECT_EQ(kSysReplFirst, "repl_first");  // src/exec/repl_log.cc reads this key via its own constant
}

TEST(ReplState, HistoryRoundTripsAndRejectsCorruption) {
  for (size_t n : {0u, 1u, 4u}) {
    std::vector<HistoryEntry> h;
    for (size_t i = 0; i < n; ++i) h.push_back({Id(static_cast<char>('a' + i)), 100 + i});
    auto back = DecodeHistory(EncodeHistory(h));
    ASSERT_TRUE(back.ok()) << n;
    EXPECT_EQ(*back, h);
  }
  const std::string good = EncodeHistory({{Id('a'), 7}, {Id('b'), 9}});
  EXPECT_EQ(DecodeHistory(good.substr(0, good.size() - 1)).status().code(),
            StatusCode::kCorruption);                                             // truncated
  EXPECT_EQ(DecodeHistory(good + "x").status().code(), StatusCode::kCorruption);  // trailing bytes
  EXPECT_EQ(DecodeHistory("").status().code(), StatusCode::kCorruption);          // no count
  std::string bad_count = good;
  bad_count[0] = static_cast<char>(kMaxHistory + 1);
  EXPECT_EQ(DecodeHistory(bad_count).status().code(), StatusCode::kCorruption);
  std::string bad_id = good;
  bad_id[1] = 'Z';
  EXPECT_EQ(DecodeHistory(bad_id).status().code(), StatusCode::kCorruption);
}

TEST(ReplState, LoadOrCreateIsStableAcrossCalls) {
  InMemoryStorage s;
  auto a = LoadOrCreateIdentity(s);
  auto b = LoadOrCreateIdentity(s);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_TRUE(IsValidReplId(a->replid));
  EXPECT_EQ(a->replid, b->replid);
  EXPECT_TRUE(a->history.empty());
}

TEST(ReplState, PersistedIdentityIsLoadedBack) {
  InMemoryStorage s;
  const ReplIdentity id{Id('c'), {{Id('d'), 5}, {Id('e'), 3}}};
  ASSERT_TRUE(PersistIdentity(s, id).ok());
  auto back = LoadOrCreateIdentity(s);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->replid, id.replid);
  EXPECT_EQ(back->history, id.history);
}

TEST(ReplState, CorruptStoredIdentityIsReported) {
  InMemoryStorage s;
  {
    auto txn = s.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, kSysReplId, "nothex").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  EXPECT_EQ(LoadOrCreateIdentity(s).status().code(), StatusCode::kCorruption);
}

TEST(ReplState, AdoptPushesTheOldIdOnlyWhenItChanges) {
  ReplIdentity me{"a" + std::string(39, '0'), {}};
  auto same = Adopt(me, me.replid, 10);
  EXPECT_TRUE(same.history.empty());
  auto other = Adopt(me, "b" + std::string(39, '0'), 10);
  EXPECT_EQ(other.replid, "b" + std::string(39, '0'));
  EXPECT_EQ(other.history, (std::vector<HistoryEntry>{{me.replid, 10}}));
}

TEST(ReplState, HistoryIsCappedNewestFirst) {
  ReplIdentity me{Id('0'), {}};
  for (char c = '1'; c <= '6'; ++c) me = Adopt(me, Id(c), static_cast<uint64_t>(c));
  EXPECT_EQ(me.replid, Id('6'));
  ASSERT_EQ(me.history.size(), kMaxHistory);
  EXPECT_EQ(me.history[0], (HistoryEntry{Id('5'), static_cast<uint64_t>('6')}));
  EXPECT_EQ(me.history[3], (HistoryEntry{Id('2'), static_cast<uint64_t>('3')}));
}

TEST(ReplState, PromoteGeneratesAFreshIdAndRecordsTheOldOne) {
  const ReplIdentity me{Id('a'), {}};
  const ReplIdentity p = Promote(me, 42);
  EXPECT_TRUE(IsValidReplId(p.replid));
  EXPECT_NE(p.replid, me.replid);
  EXPECT_EQ(p.history, (std::vector<HistoryEntry>{{me.replid, 42}}));
}

// R12 (F1): every master start takes a fresh replid, persisted, with the old one in the history up to
// the recovered last seq.
TEST(ReplState, MasterStartRotatesAndPersistsTheIdentity) {
  InMemoryStorage s;
  auto old = LoadOrCreateIdentity(s);
  ASSERT_TRUE(old.ok());
  auto rotated = RotateForMasterStart(s, *old, 90);
  ASSERT_TRUE(rotated.ok()) << rotated.status().ToString();
  EXPECT_TRUE(IsValidReplId(rotated->replid));
  EXPECT_NE(rotated->replid, old->replid);
  EXPECT_EQ(rotated->history, (std::vector<HistoryEntry>{{old->replid, 90}}));
  auto loaded = LoadOrCreateIdentity(s);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded->replid, rotated->replid);
  EXPECT_EQ(loaded->history, rotated->history);
}

TEST(ReplState, MasterStartRotationThatCannotPersistFailsAndKeepsTheStoredIdentity) {
  test::FaultyStorage s;
  auto old = LoadOrCreateIdentity(s);
  ASSERT_TRUE(old.ok());
  s.fail_commit_in = 1;
  EXPECT_FALSE(RotateForMasterStart(s, *old, 7).ok());
  auto loaded = LoadOrCreateIdentity(s);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded->replid, old->replid);
  EXPECT_TRUE(loaded->history.empty());
}

// A master that lost an unsynced tail (sync-mode interval) restarts at last 90 and re-issues 91..95 under
// its rotated id: a replica at or behind 90 continues through the history, one that applied the lost
// 91..95 must not.
TEST(ReplState, AfterAMasterRestartOnlyReplicasAtOrBehindItsRecoveredLastContinue) {
  InMemoryStorage s;
  const ReplIdentity before{Id('a'), {}};
  auto after = RotateForMasterStart(s, before, 90);
  ASSERT_TRUE(after.ok());
  const LogBounds log{1, 95};
  EXPECT_EQ(DecidePsync(*after, log, {Id('a'), 91}), PsyncDecision::kContinue);
  EXPECT_EQ(DecidePsync(*after, log, {Id('a'), 40}), PsyncDecision::kContinue);
  EXPECT_EQ(DecidePsync(*after, log, {Id('a'), 92}), PsyncDecision::kFullResync);
  EXPECT_EQ(DecidePsync(*after, log, {Id('a'), 96}), PsyncDecision::kFullResync);
}

// D8 decision table.
struct Case {
  const char* name;
  LogBounds log;
  PsyncRequest req;
  PsyncDecision want;
};
class DecidePsyncTest : public ::testing::TestWithParam<Case> {};
TEST_P(DecidePsyncTest, Decides) {
  const ReplIdentity me{std::string(40, 'a'), {{std::string(40, 'b'), 50}}};
  EXPECT_EQ(DecidePsync(me, GetParam().log, GetParam().req), GetParam().want) << GetParam().name;
}
INSTANTIATE_TEST_SUITE_P(
    Table, DecidePsyncTest,
    ::testing::Values(
        Case{"same id, caught up", {1, 100}, {std::string(40, 'a'), 101}, PsyncDecision::kContinue},
        Case{"same id, behind in backlog", {1, 100}, {std::string(40, 'a'), 1}, PsyncDecision::kContinue},
        Case{"same id, ahead of master", {1, 100}, {std::string(40, 'a'), 102}, PsyncDecision::kFullResync},
        Case{"same id, trimmed away", {60, 100}, {std::string(40, 'a'), 59}, PsyncDecision::kFullResync},
        Case{"same id, first retained", {60, 100}, {std::string(40, 'a'), 60}, PsyncDecision::kContinue},
        Case{"history id within end", {1, 100}, {std::string(40, 'b'), 51}, PsyncDecision::kContinue},
        Case{"history id past end (diverged)",
             {1, 100},
             {std::string(40, 'b'), 52},
             PsyncDecision::kFullResync},
        Case{"unknown id", {1, 100}, {std::string(40, 'c'), 10}, PsyncDecision::kFullResync},
        Case{"? from empty, nothing trimmed", {1, 100}, {"?", 1}, PsyncDecision::kContinue},
        Case{"? from empty, trimmed", {2, 100}, {"?", 1}, PsyncDecision::kFullResync},
        Case{"? with a position", {1, 100}, {"?", 5}, PsyncDecision::kFullResync},
        Case{"empty master, ? 1", {1, 0}, {"?", 1}, PsyncDecision::kContinue}));

}  // namespace
}  // namespace tideskv::repl
