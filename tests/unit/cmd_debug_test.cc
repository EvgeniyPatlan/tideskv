#include <gtest/gtest.h>

#include <string>

#include "script/sha1hex.h"
#include "support/command_harness.h"

// Expected digests were produced by redis:7.2.5 (--enable-debug-command yes) on the same commands
// (redis72-replication §10.4, plus the zset score and multi-db cases re-run for M6a Task 7).
namespace tideskv::test {
namespace {

const std::string kZeros = "+0000000000000000000000000000000000000000\r\n";

std::string Status(const std::string& hex) { return "+" + hex + "\r\n"; }

struct CmdDebugTest : ::testing::Test {
  CommandHarness h;
  std::string Run(const ArgList& a) { return h.Run(a); }
};

TEST_F(CmdDebugTest, EmptyDatasetDigestIsZeros) {
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), kZeros);
  Run({"SET", "a", "1"});
  Run({"DEL", "a"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), kZeros);  // also after deleting the last key
}

TEST_F(CmdDebugTest, StringDigestMatchesRedis) {
  Run({"SET", "a", "1"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), Status("943d800368fb9c0e951138f1bc0e313ac769d91f"));
}

TEST_F(CmdDebugTest, ListDigestMatchesRedis) {
  Run({"RPUSH", "l", "x", "1", "y"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), Status("1bf593e2db942dc07c4572fd4af005878731f300"));
}

TEST_F(CmdDebugTest, ListDigestFollowsHeadToTailOrder) {
  Run({"RPUSH", "l", "1", "y"});
  Run({"LPUSH", "l", "x"});  // the head moved left of the initial index: same list as RPUSH x 1 y
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), Status("1bf593e2db942dc07c4572fd4af005878731f300"));
}

TEST_F(CmdDebugTest, ZsetDigestMatchesRedis) {
  Run({"ZADD", "z", "1.5", "m", "2", "n"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), Status("47a9d3a7f73b0a15cad7971538cd62e431023bbd"));
}

TEST_F(CmdDebugTest, ZsetScoresUseFpconvText) {  // Redis hashes fpconv_dtoa(score), not the ZSCORE text
  Run({"ZADD", "z1", "123456789012345678", "m"});
  Run({"ZADD", "z2", "1e20", "m"});
  Run({"ZADD", "z3", "3e-7", "m"});
  Run({"ZADD", "z4", "inf", "m", "-inf", "n"});
  Run({"ZADD", "z5", "-0", "m"});
  Run({"ZADD", "z6", "0.1", "m", "1e16", "n", "100", "o"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE", "z1", "z2", "z3", "z4", "z5", "z6"}),
            "*6\r\n" + Status("27353e3c88bd44a34ab7a89eee757c71576f989a") +
                Status("b377665f5bd30745a470a82f5eae12611fe2d854") +
                Status("3bb7a68d45357a23ab84ae099e60a982f3ad955b") +
                Status("d78c237dcb98f1e641777531f4cea8b1ae056feb") +
                Status("669bd9943b43e6b31f3ab60847ee6a38dd154aba") +
                Status("97903ad23f0997d73f8ce255577a427794f48166"));
}

TEST_F(CmdDebugTest, HashDigestMatchesRedis) {
  Run({"HSET", "h", "f", "v"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), Status("ec49ca35021df6fe95cc62ee6599f1453788293b"));
}

TEST_F(CmdDebugTest, SetDigestMatchesRedis) {
  Run({"SADD", "s", "a", "b"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), Status("1aa1a03d171fe3f6470e0505beb9615dde7ca769"));
}

TEST_F(CmdDebugTest, DigestValueWithAndWithoutTtlMatchesRedis) {
  Run({"SET", "a", "1"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE", "a"}),
            "*1\r\n" + Status("b4fc66738120fba0b5aa500ab5c2bf89cea1c5da"));
  Run({"EXPIRE", "a", "100"});
  EXPECT_EQ(Run({"debug", "digest-value", "a", "nokey"}),
            "*2\r\n" + Status("5279988c5d1911f474bcbbf2f0a87012d40f8459") + kZeros);
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE"}), "*0\r\n");
}

TEST_F(CmdDebugTest, DigestValueUsesTheSelectedDb) {
  Run({"SELECT", "3"});
  Run({"SET", "a", "1"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE", "a"}),
            "*1\r\n" + Status("b4fc66738120fba0b5aa500ab5c2bf89cea1c5da"));
  Run({"SELECT", "0"});
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE", "a"}), "*1\r\n" + kZeros);
}

TEST_F(CmdDebugTest, DigestIgnoresKeyOrderAndCoversAllDbs) {
  Run({"SET", "a", "1"});
  Run({"HSET", "h", "f", "v", "g", "w"});
  Run({"SADD", "s", "x", "y", "z"});
  const std::string first = Run({"DEBUG", "DIGEST"});
  CommandHarness other;
  other.Run({"SADD", "s", "z", "x", "y"});
  other.Run({"HSET", "h", "g", "w", "f", "v"});
  other.Run({"SET", "a", "1"});
  EXPECT_EQ(other.Run({"DEBUG", "DIGEST"}), first);

  CommandHarness multi;  // a=1 in db 0 and b=2 in db 3, as in Redis
  multi.Run({"SET", "a", "1"});
  multi.Run({"SELECT", "3"});
  multi.Run({"SET", "b", "2"});
  EXPECT_EQ(multi.Run({"DEBUG", "DIGEST"}), Status("244c6ef19d30f02e83ae3141db419d893794ff5b"));
  multi.Run({"SELECT", "15"});
  multi.Run({"SET", "c", "3"});
  EXPECT_NE(multi.Run({"DEBUG", "DIGEST"}), Status("244c6ef19d30f02e83ae3141db419d893794ff5b"));
}

TEST_F(CmdDebugTest, ExpiredButStoredKeysCount) {
  Run({"SET", "a", "1"});
  Run({"PEXPIRE", "a", "100"});
  const std::string before = Run({"DEBUG", "DIGEST"});
  EXPECT_NE(before, kZeros);
  h.clock().Advance(1000);  // logically expired; no reaper runs in the harness
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}), before);
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE", "a"}),
            "*1\r\n" + Status("5279988c5d1911f474bcbbf2f0a87012d40f8459"));
}

TEST_F(CmdDebugTest, DigestValueOfMissingKeyIsZeros) {
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE", "nokey"}), "*1\r\n" + kZeros);
}

TEST_F(CmdDebugTest, UnknownSubcommandAndScriptRefusal) {
  EXPECT_EQ(Run({"DEBUG", "foo"}),
            Err("ERR unknown subcommand or wrong number of arguments for 'foo'. Try DEBUG HELP."));
  EXPECT_EQ(Run({"debug", "digest", "x"}),
            Err("ERR unknown subcommand or wrong number of arguments for 'digest'. Try DEBUG HELP."));
  EXPECT_EQ(Run({"DEBUG", std::string(200, 'x')}),
            Err("ERR unknown subcommand or wrong number of arguments for '" + std::string(128, 'x') +
                "'. Try DEBUG HELP."));
  EXPECT_EQ(Run({"DEBUG"}), Err("ERR wrong number of arguments for 'debug' command"));
  const std::string code = "return redis.call('debug','digest')";
  EXPECT_EQ(Run({"EVAL", code, "0"}), Err("ERR This Redis command is not allowed from script script: " +
                                          Sha1Hex(code) + ", on @user_script:1."));
}

TEST_F(CmdDebugTest, CorruptZsetScoreIsAnErrorReply) {
  encoding::Metadata meta;
  meta.type = encoding::ValueType::kZSet;
  meta.oid = 9;
  meta.size = 1;
  h.SeedMeta(0, "z", meta);
  auto txn = h.services.storage_.BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE(
      (*txn)->Put(ColumnFamily::kSubkey, encoding::EncodeSubkey(0, KeySlot("z"), 9, "m"), "bad").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(Run({"DEBUG", "DIGEST"}).substr(0, 1), "-");
  EXPECT_EQ(Run({"DEBUG", "DIGEST-VALUE", "z"}).substr(0, 1), "-");
}

TEST_F(CmdDebugTest, HelpNamesTheSupportedSubcommands) {
  const std::string reply = Run({"DEBUG", "HELP"});
  EXPECT_EQ(reply.substr(0, 4), "*9\r\n");
  EXPECT_NE(reply.find("+DEBUG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:\r\n"),
            std::string::npos);
  EXPECT_NE(reply.find("+DIGEST\r\n"), std::string::npos);
  EXPECT_NE(reply.find("+DIGEST-VALUE <key> [<key> ...]\r\n"), std::string::npos);
  EXPECT_NE(reply.find("+DIGEST-INDEX\r\n"), std::string::npos);
  EXPECT_NE(reply.find("+HELP\r\n"), std::string::npos);
}

}  // namespace
}  // namespace tideskv::test
