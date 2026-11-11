#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "encoding/key_codec.h"
#include "encoding/key_layout.h"
#include "encoding/meta_codec.h"
#include "encoding/object_codec.h"
#include "storage/column_family.h"
#include "support/command_harness.h"

// B1 blind SET: a plain SET skips its pre-write read when the special-key filter says the key never was a
// collection or had a TTL, and must leave exactly what the read path would.
namespace tideskv::test {
namespace {

uint64_t Skipped(CommandHarness& h) { return h.services.special_keys_.skipped_reads(); }

uint64_t OidOf(CommandHarness& h, std::string_view key) {
  const auto& meta = h.Cf(ColumnFamily::kMeta);
  auto it = meta.find(encoding::MetaKeyFor(0, key));
  if (it == meta.end()) return 0;
  auto decoded = encoding::DecodeMetadata(it->second);
  return decoded.ok() ? decoded->meta.oid : 0;
}

// Subkey entries of (db 0, key) under the collection oid it had.
size_t SubkeysOf(CommandHarness& h, std::string_view key, uint64_t oid) {
  const std::string prefix = encoding::SubkeyPrefix(0, encoding::LayoutSlot(key), oid);
  size_t n = 0;
  for (const auto& [k, v] : h.Cf(ColumnFamily::kSubkey)) n += k.starts_with(prefix) ? 1 : 0;
  return n;
}

TEST(BlindSet, BlindSetSkipsReadOnPlainKey) {
  CommandHarness h;
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(h.Run({"SET", "a", "1"}), kOk);
  EXPECT_EQ(Skipped(h), s0 + 1);
  EXPECT_EQ(h.Run({"SET", "a", "2"}), kOk);
  EXPECT_EQ(Skipped(h), s0 + 2);
  EXPECT_EQ(h.Run({"GET", "a"}), Bulk("2"));
}

TEST(BlindSet, SetOnHashKeyDropsFields) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"HSET", "h", "f", "v"}), Int(1));
  const uint64_t oid = OidOf(h, "h");
  ASSERT_NE(oid, 0u);
  ASSERT_EQ(SubkeysOf(h, "h", oid), 1u);
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(h.Run({"SET", "h", "x"}), kOk);
  EXPECT_EQ(Skipped(h), s0);
  EXPECT_EQ(h.Run({"TYPE", "h"}), "+string\r\n");
  EXPECT_EQ(SubkeysOf(h, "h", oid), 0u);
}

TEST(BlindSet, SetOnTtlKeyClearsExpireEntry) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"SET", "t", "v", "EX", "100"}), kOk);
  ASSERT_EQ(h.Cf(ColumnFamily::kExpire).size(), 1u);
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(h.Run({"SET", "t", "w"}), kOk);
  EXPECT_EQ(Skipped(h), s0);
  EXPECT_EQ(h.Run({"TTL", "t"}), Int(-1));
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
}

TEST(BlindSet, SetAfterPersistStillReads) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"SET", "p", "v", "EX", "100"}), kOk);
  ASSERT_EQ(h.Run({"PERSIST", "p"}), Int(1));
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(h.Run({"SET", "p", "w"}), kOk);
  EXPECT_EQ(Skipped(h), s0);
  EXPECT_EQ(h.Run({"GET", "p"}), Bulk("w"));
  EXPECT_EQ(h.Run({"TTL", "p"}), Int(-1));
}

TEST(BlindSet, SetAfterHsetInSameExecDropsFields) {
  CommandHarness h;
  const uint64_t s0 = Skipped(h);
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  h.Run({"HSET", "k", "f", "v"});
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Int(1), kOk}));
  EXPECT_EQ(Skipped(h), s0);
  EXPECT_EQ(h.Run({"TYPE", "k"}), "+string\r\n");
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST(BlindSet, LuaHsetThenSetDropsFields) {
  CommandHarness h;
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(
      h.Run({"EVAL", "redis.call('HSET',KEYS[1],'f','v'); return redis.call('SET',KEYS[1],'x')", "1", "k"}),
      kOk);
  EXPECT_EQ(Skipped(h), s0);
  EXPECT_EQ(h.Run({"TYPE", "k"}), "+string\r\n");
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST(BlindSet, BlindSetPastDeadlineFallsBack) {
  CommandHarness a;
  CommandHarness b;
  ASSERT_TRUE(a.services.repl_log_.ActivateAndSnapshot().ok());
  ASSERT_TRUE(b.services.repl_log_.ActivateAndSnapshot().ok());
  ASSERT_EQ(b.Run({"CONFIG", "SET", "blind-set", "no"}), kOk);
  const uint64_t s0 = Skipped(a);
  EXPECT_EQ(a.Run({"SET", "a", "v", "EXAT", "1"}), kOk);
  EXPECT_EQ(b.Run({"SET", "a", "v", "EXAT", "1"}), kOk);
  EXPECT_EQ(Skipped(a), s0);
  EXPECT_TRUE(a.Cf(ColumnFamily::kMeta).empty());
  EXPECT_EQ(a.Cf(ColumnFamily::kReplog), b.Cf(ColumnFamily::kReplog));
}

TEST(BlindSet, NotReadyMeansRead) {
  CommandHarness h;
  h.services.special_keys_.Reset(16);
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(h.Run({"SET", "a", "1"}), kOk);
  EXPECT_EQ(Skipped(h), s0);
  h.services.special_keys_.MarkReady();
  EXPECT_EQ(h.Run({"SET", "b", "1"}), kOk);
  EXPECT_EQ(Skipped(h), s0 + 1);
}

TEST(BlindSet, DisabledMeansRead) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"CONFIG", "SET", "blind-set", "no"}), kOk);
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(h.Run({"SET", "a", "1"}), kOk);
  EXPECT_EQ(Skipped(h), s0);
  EXPECT_EQ(h.Run({"GET", "a"}), Bulk("1"));
}

TEST(BlindSet, ConditionalAndGetFormsRead) {
  CommandHarness h;
  const std::vector<ArgList> cmds = {
      {"SET", "a", "1", "NX"},      {"SET", "a", "1", "XX"}, {"SET", "a", "1", "GET"},
      {"SET", "a", "1", "KEEPTTL"}, {"GETSET", "a", "1"},    {"SETNX", "a", "1"},
      {"MSETNX", "x", "1"},
  };
  for (const ArgList& c : cmds) {
    const uint64_t s0 = Skipped(h);
    const std::string r = h.Run(c);
    EXPECT_FALSE(r.starts_with("-")) << c[0] << " -> " << r;
    EXPECT_EQ(Skipped(h), s0) << c[0] << " " << (c.size() > 3 ? c[3] : "");
  }
}

TEST(BlindSet, MsetSkipsReadPerKey) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"HSET", "h", "f", "v"}), Int(1));
  const uint64_t oid = OidOf(h, "h");
  const uint64_t s0 = Skipped(h);
  EXPECT_EQ(h.Run({"MSET", "a", "1", "h", "2"}), kOk);
  EXPECT_EQ(Skipped(h), s0 + 1);
  EXPECT_EQ(h.Run({"TYPE", "h"}), "+string\r\n");
  EXPECT_EQ(SubkeysOf(h, "h", oid), 0u);
  EXPECT_EQ(h.Run({"MGET", "a", "h"}), Arr({Bulk("1"), Bulk("2")}));
}

void ExpectWatchOutcome(bool existing, bool watch_same_key, const std::string& expected_exec) {
  CommandHarness h;
  SecondClient other(h);
  if (existing) {
    ASSERT_EQ(h.Run({"SET", "k", "old"}), kOk);
  }
  ASSERT_EQ(h.Run({"WATCH", watch_same_key ? "k" : "other"}), kOk);
  const uint64_t s0 = Skipped(h);
  ASSERT_EQ(other.Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Skipped(h), s0 + 1);
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  h.Run({"GET", "k"});
  EXPECT_EQ(h.Run({"EXEC"}), expected_exec) << "existing=" << existing << " same=" << watch_same_key;
}

TEST(BlindSet, WatchSeesBlindSet) {
  ExpectWatchOutcome(/*existing=*/false, /*watch_same_key=*/true, "*-1\r\n");
  ExpectWatchOutcome(/*existing=*/true, /*watch_same_key=*/true, "*-1\r\n");
  ExpectWatchOutcome(/*existing=*/false, /*watch_same_key=*/false, Arr({Bulk("v")}));
}

TEST(BlindSet, DbsizeCountsBlindSet) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(0));
  EXPECT_EQ(h.Run({"SET", "n", "1"}), kOk);
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(1));
  EXPECT_EQ(h.Run({"SET", "n", "2"}), kOk);
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(1));
}

// One random command over 40 keys, or a clock move ({"@advance", ms}).
ArgList RandomCommand(std::mt19937_64& rng) {
  auto pick = [&](uint64_t n) { return static_cast<int>(rng() % n); };
  auto key = [&] { return "k" + std::to_string(pick(40)); };
  auto val = [&] { return "v" + std::to_string(pick(1000)); };
  auto secs = [&] { return std::to_string(1 + pick(5)); };
  auto millis = [&] { return std::to_string(1 + pick(5000)); };
  switch (pick(22)) {
    case 0:
    case 1:
    case 2:
    case 3:
      return {"SET", key(), val()};
    case 4:
      return {"SET", key(), val(), "EX", secs()};
    case 5:
      return {"SET", key(), val(), "PX", millis()};
    case 6:
      return {"SET", key(), val(), "EXAT", "1"};
    case 7: {
      static const std::vector<std::string> kOpts = {"KEEPTTL", "NX", "XX", "GET"};
      return {"SET", key(), val(), kOpts[static_cast<size_t>(pick(kOpts.size()))]};
    }
    case 8:
      return {"SETEX", key(), secs(), val()};
    case 9:
      return {"PSETEX", key(), millis(), val()};
    case 10: {
      ArgList m = {"MSET"};
      const int pairs = 2 + pick(2);
      for (int i = 0; i < pairs; ++i) {
        m.push_back(key());
        m.push_back(val());
      }
      return m;
    }
    case 11:
      return {"HSET", key(), "f" + std::to_string(pick(3)), val()};
    case 12:
      return {"SADD", key(), "m" + std::to_string(pick(3))};
    case 13:
      return {"ZADD", key(), std::to_string(pick(10)), "m" + std::to_string(pick(3))};
    case 14:
      return {"RPUSH", key(), val()};
    case 15:
      return {"LPOP", key()};
    case 16:
      return {"EXPIRE", key(), secs()};
    case 17:
      return {"PERSIST", key()};
    case 18:
      return {"DEL", key()};
    case 19:
      return {"RENAME", key(), key()};
    case 20: {
      if (pick(3) == 0) return {"INCR", key()};
      if (pick(2) == 0) return {"APPEND", key(), val()};
      return {"GETEX", key(), "EX", secs()};
    }
    default:
      return {"@advance", std::to_string(pick(2001))};
  }
}

std::string Describe(const ArgList& argv) {
  std::string out;
  for (const auto& a : argv) out += (out.empty() ? "" : " ") + a;
  return out;
}

std::string IndexCounts(const std::string& digest_index) {  // the trailing ":entries :missing :orphans"
  const size_t first = digest_index.find(":");
  return first == std::string::npos ? digest_index : digest_index.substr(first);
}

void RunDifferential(uint64_t seed) {
  CommandHarness a;  // blind-set yes (the default)
  CommandHarness b;  // blind-set no: today's read path
  ASSERT_TRUE(a.services.repl_log_.ActivateAndSnapshot().ok());
  ASSERT_TRUE(b.services.repl_log_.ActivateAndSnapshot().ok());
  ASSERT_EQ(b.Run({"CONFIG", "SET", "blind-set", "no"}), kOk);
  const uint64_t s0 = Skipped(a);
  std::mt19937_64 rng(seed);
  for (int i = 0; i < 2000; ++i) {
    const ArgList cmd = RandomCommand(rng);
    if (cmd[0] == "@advance") {
      const int64_t ms = std::stoll(cmd[1]);
      a.clock().Advance(ms);
      b.clock().Advance(ms);
      continue;
    }
    const std::string ra = a.Run(cmd);
    const std::string rb = b.Run(cmd);
    ASSERT_EQ(ra, rb) << "seed " << seed << " command " << i << ": " << Describe(cmd);
  }
  for (ColumnFamily cf : kAllColumnFamilies) {
    EXPECT_EQ(a.Cf(cf), b.Cf(cf)) << "seed " << seed << " cf " << static_cast<int>(cf);
  }
  const std::string da = a.Run({"DEBUG", "DIGEST-INDEX"});
  EXPECT_EQ(da, b.Run({"DEBUG", "DIGEST-INDEX"})) << "seed " << seed;
  EXPECT_TRUE(da.ends_with(":0\r\n:0\r\n")) << "seed " << seed << ": " << IndexCounts(da);
  const std::string sk = a.Run({"DEBUG", "SPECIAL-KEYS"});
  EXPECT_TRUE(sk.ends_with(":0\r\n")) << "seed " << seed << ": " << sk;
  const uint64_t skipped = Skipped(a) - s0;
  EXPECT_GT(skipped, 0u) << "seed " << seed;
  EXPECT_EQ(Skipped(b), 0u) << "seed " << seed;
  std::cout << "[ differential ] seed " << seed << ": " << skipped << " reads skipped\n";
}

TEST(BlindSet, DifferentialMatchesReadPath) {
  for (uint64_t seed : {1u, 2u, 3u}) RunDifferential(seed);
}

}  // namespace
}  // namespace tideskv::test
