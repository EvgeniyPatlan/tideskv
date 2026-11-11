#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "encoding/meta_codec.h"
#include "repl/full_sync_frames.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/key_layout_skip.h"
#include "support/migration_harness.h"
#include "support/replica_stack.h"
#include "support/snapshot_frames.h"
#include "types/special_key_filter.h"

// B1 coverage: every path that writes a meta record feeds the special-key filter. Task 6 lets a plain SET
// skip its read when the filter says a key never was a collection or had a TTL, so a writer the filter does
// not see would let SET corrupt data. With a 1M-key filter and these datasets (< 10k keys) a false positive
// hiding a gap has odds below 1e-10 per key.
namespace tideskv::test {
namespace {

SpecialKeyCheck Check(Storage& storage, const SpecialKeyFilter& filter) {
  auto view = storage.BeginRead();
  EXPECT_TRUE(view.ok()) << view.status().ToString();
  if (!view.ok()) return {};
  auto c = CheckSpecialKeys(**view, filter);
  EXPECT_TRUE(c.ok()) << c.status().ToString();
  return c.ok() ? *c : SpecialKeyCheck{};
}

SpecialKeyCheck Check(CommandHarness& h) { return Check(h.services.storage_, h.services.special_keys_); }

// Forwards every meta Put to the harness's filter and to a filter of its own, so a test can tell what
// one command fed the filter.
class TeeHook final : public MetaPutHook {
 public:
  explicit TeeHook(MetaPutHook& inner) : inner_(inner) {}
  void OnMetaPut(std::string_view meta_key, std::string_view value) noexcept override {
    inner_.OnMetaPut(meta_key, value);
    seen.OnMetaPut(meta_key, value);
  }
  SpecialKeyFilter seen{1u << 16};

 private:
  MetaPutHook& inner_;
};

struct Case {
  std::vector<ArgList> setup;
  ArgList command;
  bool creating;
};

// command name -> its cases (setup, then the command). creating: the command itself leaves a special key.
const std::map<std::string, std::vector<Case>>& Cases() {
  static const std::map<std::string, std::vector<Case>> kCases = [] {
    const std::vector<ArgList> hash = {{"HSET", "k", "f", "v", "g", "w"}};
    const std::vector<ArgList> set = {{"SADD", "k", "a", "b", "c"}};
    const std::vector<ArgList> zset = {{"ZADD", "k", "1", "a", "2", "b", "3", "c"}};
    const std::vector<ArgList> list = {{"RPUSH", "k", "a", "b", "c"}};
    const std::vector<ArgList> two_sets = {{"SADD", "a", "x"}, {"SADD", "b", "x"}};
    const std::vector<ArgList> two_zsets = {{"ZADD", "za", "1", "m"}, {"ZADD", "zb", "1", "m"}};
    const std::vector<ArgList> src = {{"RPUSH", "src", "a", "b"}};
    const std::vector<ArgList> plain = {{"SET", "k", "v"}};
    const std::vector<ArgList> ttl_num = {{"SET", "t", "1", "EX", "100"}};
    std::map<std::string, std::vector<Case>> m;
    auto add = [&m](const std::string& name, std::vector<ArgList> s, ArgList cmd, bool creating) {
      m[name].push_back({std::move(s), std::move(cmd), creating});
    };
    // Hash
    add("hset", {}, {"HSET", "k", "f", "v"}, true);
    add("hsetnx", {}, {"HSETNX", "k", "f", "v"}, true);
    add("hmset", {}, {"HMSET", "k", "f", "v"}, true);
    add("hincrby", {}, {"HINCRBY", "k", "f", "2"}, true);
    add("hincrbyfloat", {}, {"HINCRBYFLOAT", "k", "f", "1.5"}, true);
    add("hdel", hash, {"HDEL", "k", "f"}, true);
    add("hdel", hash, {"HDEL", "k", "f", "g"}, false);
    // Set
    add("sadd", {}, {"SADD", "k", "m"}, true);
    add("smove", {{"SADD", "s1", "m"}}, {"SMOVE", "s1", "s2", "m"}, true);
    add("sdiffstore", {{"SADD", "a", "x", "y"}, {"SADD", "b", "x"}}, {"SDIFFSTORE", "d", "a", "b"}, true);
    add("sinterstore", two_sets, {"SINTERSTORE", "d", "a", "b"}, true);
    add("sunionstore", two_sets, {"SUNIONSTORE", "d", "a", "b"}, true);
    add("srem", set, {"SREM", "k", "a"}, true);
    add("spop", set, {"SPOP", "k"}, true);
    // Zset
    add("zadd", {}, {"ZADD", "k", "1", "m"}, true);
    add("zincrby", {}, {"ZINCRBY", "k", "1", "m"}, true);
    add("zdiffstore", {{"ZADD", "za", "1", "m", "2", "n"}, {"ZADD", "zb", "1", "m"}},
        {"ZDIFFSTORE", "zd", "2", "za", "zb"}, true);
    add("zinterstore", two_zsets, {"ZINTERSTORE", "zd", "2", "za", "zb"}, true);
    add("zunionstore", two_zsets, {"ZUNIONSTORE", "zd", "2", "za", "zb"}, true);
    add("zrangestore", {{"ZADD", "za", "1", "m"}}, {"ZRANGESTORE", "zd", "za", "0", "-1"}, true);
    add("zrem", zset, {"ZREM", "k", "a"}, true);
    add("zremrangebylex", {{"ZADD", "k", "0", "a", "0", "b", "0", "c"}}, {"ZREMRANGEBYLEX", "k", "[a", "[a"},
        true);
    add("zremrangebyrank", zset, {"ZREMRANGEBYRANK", "k", "0", "0"}, true);
    add("zremrangebyscore", zset, {"ZREMRANGEBYSCORE", "k", "1", "1"}, true);
    add("zpopmin", zset, {"ZPOPMIN", "k"}, true);
    add("zpopmax", zset, {"ZPOPMAX", "k"}, true);
    add("zmpop", zset, {"ZMPOP", "1", "k", "MIN"}, true);
    add("bzpopmin", zset, {"BZPOPMIN", "k", "0.01"}, true);
    add("bzpopmax", zset, {"BZPOPMAX", "k", "0.01"}, true);
    add("bzmpop", zset, {"BZMPOP", "0.01", "1", "k", "MAX"}, true);
    // List
    add("lpush", {}, {"LPUSH", "k", "a"}, true);
    add("rpush", {}, {"RPUSH", "k", "a"}, true);
    add("lpushx", list, {"LPUSHX", "k", "z"}, true);
    add("rpushx", list, {"RPUSHX", "k", "z"}, true);
    add("linsert", list, {"LINSERT", "k", "BEFORE", "b", "z"}, true);
    add("lset", list, {"LSET", "k", "0", "z"}, true);
    add("ltrim", list, {"LTRIM", "k", "0", "1"}, true);
    add("lrem", list, {"LREM", "k", "0", "a"}, true);
    add("lpop", list, {"LPOP", "k"}, true);
    add("rpop", list, {"RPOP", "k"}, true);
    add("lmpop", list, {"LMPOP", "1", "k", "LEFT"}, true);
    add("blpop", list, {"BLPOP", "k", "0.01"}, true);
    add("brpop", list, {"BRPOP", "k", "0.01"}, true);
    add("blmpop", list, {"BLMPOP", "0.01", "1", "k", "RIGHT"}, true);
    add("lmove", src, {"LMOVE", "src", "dst", "LEFT", "RIGHT"}, true);
    add("rpoplpush", src, {"RPOPLPUSH", "src", "dst"}, true);
    add("blmove", src, {"BLMOVE", "src", "dst", "RIGHT", "LEFT", "0.01"}, true);
    add("brpoplpush", src, {"BRPOPLPUSH", "src", "dst", "0.01"}, true);
    // TTL
    add("expire", plain, {"EXPIRE", "k", "100"}, true);
    add("pexpire", plain, {"PEXPIRE", "k", "100000"}, true);
    add("expireat", plain, {"EXPIREAT", "k", "1800000000"}, true);
    add("pexpireat", plain, {"PEXPIREAT", "k", "1800000000000"}, true);
    add("set", {}, {"SET", "k", "v", "EX", "100"}, true);
    add("set", plain, {"SET", "k", "w", "PX", "100000"}, true);
    add("setex", {}, {"SETEX", "k", "100", "v"}, true);
    add("psetex", {}, {"PSETEX", "k", "100000", "v"}, true);
    add("getex", plain, {"GETEX", "k", "EX", "100"}, true);
    add("rename", hash, {"RENAME", "k", "r"}, true);
    add("rename", {{"SET", "k", "v", "EX", "100"}}, {"RENAME", "k", "r"}, true);
    add("renamenx", hash, {"RENAMENX", "k", "r"}, true);
    add("renamenx", {{"SET", "k", "v", "EX", "100"}}, {"RENAMENX", "k", "r"}, true);
    add("incr", ttl_num, {"INCR", "t"}, true);
    add("incrby", ttl_num, {"INCRBY", "t", "5"}, true);
    add("decr", ttl_num, {"DECR", "t"}, true);
    add("decrby", ttl_num, {"DECRBY", "t", "5"}, true);
    add("incrbyfloat", ttl_num, {"INCRBYFLOAT", "t", "1.5"}, true);
    add("append", ttl_num, {"APPEND", "t", "x"}, true);
    add("setrange", ttl_num, {"SETRANGE", "t", "3", "x"}, true);
    // Non-creating: each removes or rewrites what a creating setup left.
    add("set", hash, {"SET", "k", "v"}, false);
    add("set", ttl_num, {"SET", "t", "v"}, false);
    add("setnx", hash, {"SETNX", "n", "v"}, true);  // the hash stays
    add("setnx", {}, {"SETNX", "n", "v"}, false);
    add("getset", ttl_num, {"GETSET", "t", "v"}, false);
    add("mset", {{"HSET", "a", "f", "v"}, {"SET", "b", "v", "EX", "100"}}, {"MSET", "a", "1", "b", "2"},
        false);
    add("msetnx", hash, {"MSETNX", "x", "1", "y", "2"}, true);  // the hash stays
    add("msetnx", {}, {"MSETNX", "x", "1", "y", "2"}, false);
    add("persist", ttl_num, {"PERSIST", "t"}, false);
    add("del", hash, {"DEL", "k"}, false);
    add("del", ttl_num, {"DEL", "t"}, false);
    add("unlink", zset, {"UNLINK", "k"}, false);
    add("getdel", ttl_num, {"GETDEL", "t"}, false);
    add("flushdb", {{"HSET", "a", "f", "v"}, {"SET", "b", "v", "EX", "100"}}, {"FLUSHDB"}, false);
    add("flushall", {{"ZADD", "a", "1", "m"}, {"SET", "b", "v", "EX", "100"}}, {"FLUSHALL"}, false);
    return m;
  }();
  return kCases;
}

std::string Describe(const ArgList& argv) {
  std::string out;
  for (const auto& a : argv) out += (out.empty() ? "" : " ") + a;
  return out;
}

TEST(SpecialKeyCoverage, EveryWriteCommandIsClassified) {
  CommandHarness h;
  std::string unclassified;
  for (const auto& [name, spec] : h.services.registry().all()) {
    if ((spec.flags & kCmdWrite) != 0 && !Cases().contains(name)) unclassified += " " + name;
  }
  EXPECT_TRUE(unclassified.empty()) << "write commands with no special-key case:" << unclassified;
  for (const auto& [name, cases] : Cases()) {
    const CommandSpec* spec = h.services.registry().Find(name);
    EXPECT_NE(spec, nullptr) << name << " is not registered";
  }
}

// Runs the case on a fresh harness. Asserts the filter is complete afterwards and, stronger, that every
// meta record the command itself wrote special reached the hook during the command (so the case does not
// pass merely because its setup already put the key in the filter).
void RunCase(const std::string& name, const Case& c) {
  const std::string what = name + ": " + Describe(c.command);
  CommandHarness h;
  for (const auto& s : c.setup) {
    const std::string r = h.Run(s);
    ASSERT_FALSE(r.starts_with("-")) << what << " setup " << Describe(s) << " -> " << r;
  }
  const InMemoryStorage::Table before = h.Cf(ColumnFamily::kMeta);
  TeeHook tee(h.services.special_keys_);
  h.services.storage_.SetMetaPutHook(&tee);
  const std::string r = h.Run(c.command);
  h.services.storage_.SetMetaPutHook(&h.services.special_keys_);
  ASSERT_FALSE(r.starts_with("-")) << what << " -> " << r;

  const SpecialKeyCheck check = Check(h);
  EXPECT_EQ(check.missing, 0u) << what;
  if (c.creating) {
    EXPECT_GT(check.specials, 0u) << what;
  } else {
    EXPECT_EQ(check.specials, 0u) << what;
  }
  int changed_specials = 0;
  for (const auto& [key, value] : h.Cf(ColumnFamily::kMeta)) {
    auto old = before.find(key);
    if ((old != before.end() && old->second == value) || !encoding::MetaIsSpecial(value)) continue;
    ++changed_specials;
    EXPECT_TRUE(tee.seen.MayContain(key)) << what << ": its special write of a meta record bypassed the hook";
  }
  const bool from_empty =
      std::all_of(c.setup.begin(), c.setup.end(), [](const ArgList& s) { return s[0] == "SELECT"; });
  if (c.creating && from_empty) {
    EXPECT_GT(changed_specials, 0) << what;
  }
}

TEST(SpecialKeyCoverage, EveryCaseKeepsFilterComplete) {
  for (const auto& [name, cases] : Cases()) {
    for (const Case& c : cases) RunCase(name, c);
  }
}

// The same cases in database 3: the filter's key is the encoded meta key, db included.
TEST(SpecialKeyCoverage, EveryCaseKeepsFilterCompleteInAnotherDb) {
  for (const auto& [name, cases] : Cases()) {
    for (const Case& c : cases) {
      Case in_db3 = c;
      in_db3.setup.insert(in_db3.setup.begin(), ArgList{"SELECT", "3"});
      RunCase(name, in_db3);
    }
  }
}

TEST(SpecialKeyCoverage, MultiExecKeepsFilterComplete) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  h.Run({"HSET", "m", "f", "v"});
  h.Run({"SET", "m2", "v", "EX", "100"});
  h.Run({"ZADD", "m3", "1", "a"});
  const std::string exec = h.Run({"EXEC"});
  EXPECT_EQ(exec, Arr({Int(1), kOk, Int(1)}));
  SpecialKeyCheck c = Check(h);
  EXPECT_EQ(c.missing, 0u);
  EXPECT_EQ(c.specials, 3u);

  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  h.Run({"HSET", "d1", "f", "v"});
  h.Run({"RPUSH", "d2", "a"});
  ASSERT_EQ(h.Run({"DISCARD"}), kOk);
  c = Check(h);
  EXPECT_EQ(c.missing, 0u);
  EXPECT_EQ(c.specials, 3u);

  // One queued command fails at run time (WRONGTYPE); the others still apply.
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  h.Run({"SADD", "e1", "a"});
  h.Run({"INCR", "m"});
  h.Run({"PEXPIRE", "e1", "100000"});
  h.Run({"RPUSH", "e2", "a"});
  const std::string failed = h.Run({"EXEC"});
  EXPECT_NE(failed.find("WRONGTYPE"), std::string::npos) << failed;
  c = Check(h);
  EXPECT_EQ(c.missing, 0u);
  EXPECT_EQ(c.specials, 5u);
}

TEST(SpecialKeyCoverage, LuaKeepsFilterComplete) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"SET", "ls", "v"}), kOk);
  EXPECT_EQ(
      h.Run({"EVAL", "redis.call('HSET',KEYS[1],'f','v'); redis.call('PEXPIRE',KEYS[2],100000); return 1",
             "2", "lh", "ls"}),
      Int(1));
  SpecialKeyCheck c = Check(h);
  EXPECT_EQ(c.missing, 0u);
  EXPECT_EQ(c.specials, 2u);

  const std::string err = h.Run({"EVAL", "redis.call('HSET',KEYS[1],'f','v'); error('x')", "1", "le"});
  EXPECT_TRUE(err.starts_with("-")) << err;
  c = Check(h);
  EXPECT_EQ(c.missing, 0u);
  EXPECT_GE(c.specials, 2u);
}

// 200 mixed commands that each create or keep a special key (some through moves, stores and renames).
void FillMixed(CommandHarness& h) {
  for (int i = 0; i < 200; ++i) {
    const std::string n = std::to_string(i);
    std::string r;
    switch (i % 10) {
      case 0:
        r = h.Run({"HSET", "h" + n, "f", "v"});
        break;
      case 1:
        r = h.Run({"SADD", "s" + n, "a", "b"});
        break;
      case 2:
        r = h.Run({"ZADD", "z" + n, "1", "a"});
        break;
      case 3:
        r = h.Run({"RPUSH", "l" + n, "a", "b"});
        break;
      case 4:
        r = h.Run({"SET", "t" + n, "v", "EX", "1000"});
        break;
      case 5:
        r = h.Run({"SET", "p" + n, "v"});
        if (r == kOk) r = h.Run({"PEXPIRE", "p" + n, "1000000"});
        break;
      case 6:
        r = h.Run({"LMOVE", "l" + std::to_string(i - 3), "m" + n, "LEFT", "RIGHT"});
        break;
      case 7:
        r = h.Run({"ZUNIONSTORE", "u" + n, "1", "z" + std::to_string(i - 5)});
        break;
      case 8:
        r = h.Run({"RENAME", "h" + std::to_string(i - 8), "rh" + n});
        break;
      default:
        r = h.Run({"SET", "plain" + n, "v"});
        break;
    }
    ASSERT_FALSE(r.find("-") == 0) << i << ": " << r;
  }
}

TEST(SpecialKeyCoverage, ReplicaApplyKeepsFilterComplete) {
  CommandHarness master;
  ASSERT_TRUE(master.services.repl_log_.ActivateAndSnapshot().ok());
  FillMixed(master);
  const SpecialKeyCheck m = Check(master);
  ASSERT_EQ(m.missing, 0u);
  ASSERT_EQ(m.specials, 160u);  // cases 0-7 add one special key each; RENAME moves one, a plain SET none

  SpecialKeyFilter filter;  // before the storage: its txns hold the hook pointer
  InMemoryStorage storage;
  storage.SetMetaPutHook(&filter);
  filter.MarkReady();
  ReplicaStack replica(storage);
  ASSERT_TRUE(ShipAll(master.services.storage_, master.services.repl_log_.last(), replica.engine).ok());
  const SpecialKeyCheck r = Check(storage, filter);
  EXPECT_EQ(r.missing, 0u);
  EXPECT_EQ(r.specials, m.specials);
  EXPECT_EQ(DigestOf(replica.engine), DigestOf(master.services.engine_));
}

TEST(SpecialKeyCoverage, FullSyncLoadKeepsFilterComplete) {
  CommandHarness master;
  FillMixed(master);
  const SpecialKeyCheck m = Check(master);
  ASSERT_EQ(m.missing, 0u);
  ASSERT_EQ(m.specials, 160u);

  SpecialKeyFilter filter;
  InMemoryStorage storage;
  storage.SetMetaPutHook(&filter);
  filter.MarkReady();
  ReplicaStack replica(storage);
  const SnapshotStream stream = SnapshotFrames(master.services.repl_log_);
  ASSERT_GE(stream.frames.size(), 2u);
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  for (const auto& f : stream.frames) {
    if (f[0] != "LOAD") continue;
    auto ops = repl::DecodeLoadChunk(f[1]);
    ASSERT_TRUE(ops.ok()) << ops.status().ToString();
    ASSERT_TRUE(replica.engine.LoadChunk(*ops).ok());
  }
  const auto& end = stream.frames.back();
  ASSERT_EQ(end[0], "LOADEND");
  ASSERT_TRUE(replica.engine.EndLoad(stream.seq, end[2], {}).ok());
  const SpecialKeyCheck r = Check(storage, filter);
  EXPECT_EQ(r.missing, 0u);
  EXPECT_EQ(r.specials, m.specials);
  EXPECT_EQ(DigestOf(replica.engine), DigestOf(master.services.engine_));
}

TEST(SpecialKeyCoverage, SlotMigrationImportKeepsFilterComplete) {
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  // {b} hashes to slot 3300, which A owns.
  ASSERT_EQ(p.source.Run({"HSET", "{b}h", "f", "v"}), Int(1));
  ASSERT_EQ(p.source.Run({"ZADD", "{b}z", "1", "m"}), Int(1));
  ASSERT_EQ(p.source.Run({"RPUSH", "{b}l", "a"}), Int(1));
  ASSERT_EQ(p.source.Run({"SET", "{b}t", "v", "EX", "1000"}), kOk);
  ASSERT_EQ(p.source.Run({"SET", "{b}plain", "v"}), kOk);
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  const SpecialKeyCheck t = Check(p.target);
  EXPECT_EQ(t.missing, 0u);
  EXPECT_EQ(t.specials, 4u);
}

}  // namespace
}  // namespace tideskv::test
