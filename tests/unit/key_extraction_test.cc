#include "commands/key_extraction.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "support/command_harness.h"

namespace tideskv::test {
namespace {

std::vector<std::string> Keys(CommandHarness& h, const ArgList& argv) {
  const CommandSpec* spec = h.services.registry().Find(argv[0]);
  EXPECT_NE(spec, nullptr) << argv[0];
  std::vector<std::string> out;
  if (spec == nullptr) return out;
  for (size_t i : KeyIndexes(*spec, argv)) out.push_back(argv[i]);
  return out;
}

using V = std::vector<std::string>;

TEST(KeyExtraction, TriplesCoverFixedPositions) {
  CommandHarness h;
  EXPECT_EQ(Keys(h, {"GET", "k"}), V{"k"});
  EXPECT_EQ(Keys(h, {"MSET", "a", "1", "b", "2"}), (V{"a", "b"}));
  EXPECT_EQ(Keys(h, {"BLPOP", "l1", "l2", "0"}), (V{"l1", "l2"}));
  EXPECT_EQ(Keys(h, {"RENAME", "a", "b"}), (V{"a", "b"}));
  EXPECT_EQ(Keys(h, {"DEL", "a", "b", "c"}), (V{"a", "b", "c"}));
  EXPECT_TRUE(Keys(h, {"PING"}).empty());
}

TEST(KeyExtraction, NumkeysCommands) {  // Redis genericGetKeys (db.c:2209)
  CommandHarness h;
  EXPECT_EQ(Keys(h, {"SINTERCARD", "2", "a", "b", "LIMIT", "1"}), (V{"a", "b"}));
  EXPECT_EQ(Keys(h, {"ZUNION", "2", "a", "b", "WEIGHTS", "1", "2"}), (V{"a", "b"}));
  EXPECT_EQ(Keys(h, {"ZINTERCARD", "1", "a"}), V{"a"});
  EXPECT_EQ(Keys(h, {"LMPOP", "2", "a", "b", "LEFT"}), (V{"a", "b"}));
  EXPECT_EQ(Keys(h, {"ZMPOP", "1", "z", "MIN"}), V{"z"});
  EXPECT_EQ(Keys(h, {"BLMPOP", "0", "2", "a", "b", "LEFT"}), (V{"a", "b"}));
  EXPECT_EQ(Keys(h, {"BZMPOP", "0", "1", "z", "MAX"}), V{"z"});
  EXPECT_EQ(Keys(h, {"EVAL", "return 1", "2", "k1", "k2", "arg"}), (V{"k1", "k2"}));
  EXPECT_EQ(Keys(h, {"EVALSHA_RO", std::string(40, 'a'), "1", "k"}), V{"k"});
  EXPECT_TRUE(Keys(h, {"EVAL", "return 1", "0"}).empty());
}

TEST(KeyExtraction, StoreCommandsIncludeTheirSources) {  // the 1,1,1 triple dropped them
  CommandHarness h;
  EXPECT_EQ(Keys(h, {"ZUNIONSTORE", "d", "2", "a", "b", "AGGREGATE", "SUM"}), (V{"d", "a", "b"}));
  EXPECT_EQ(Keys(h, {"ZINTERSTORE", "d", "1", "a"}), (V{"d", "a"}));
  EXPECT_EQ(Keys(h, {"ZDIFFSTORE", "d", "2", "a", "b"}), (V{"d", "a", "b"}));
}

TEST(KeyExtraction, ABadNumkeysGivesNoKeysSoTheCommandAnswersItsOwnError) {
  CommandHarness h;
  EXPECT_TRUE(Keys(h, {"SINTERCARD", "x", "a"}).empty());
  EXPECT_TRUE(Keys(h, {"SINTERCARD", "0", "a"}).empty());
  EXPECT_TRUE(Keys(h, {"SINTERCARD", "5", "a"}).empty());
  EXPECT_TRUE(Keys(h, {"ZUNIONSTORE", "d", "x", "a"}).empty());  // no destination either (Redis)
  EXPECT_TRUE(Keys(h, {"EVAL", "return 1", "-1"}).empty());
}

// D17 drift guard: a command registered with no key spec is either keyless or in the extractor table.
TEST(KeyExtraction, EveryCommandWithoutAKeySpecIsKnown) {
  CommandHarness h;
  const std::set<std::string> keyless = {
      "asking",  "auth",  "client", "cluster",   "clusterx", "command",   "config",   "dbsize",    "debug",
      "discard", "echo",  "exec",   "flushall",  "flushdb",  "hello",     "info",     "keys",      "multi",
      "ping",    "psync", "quit",   "randomkey", "readonly", "readwrite", "replconf", "replicaof", "reset",
      "role",    "scan",  "script", "select",    "shutdown", "slaveof",   "sync",     "unwatch",   "wait"};
  for (const auto& [name, spec] : h.services.registry().all()) {
    if (spec.first_key != 0) continue;
    EXPECT_TRUE(keyless.count(name) == 1 || HasMovableKeys(name))
        << name << " has no key spec and no extractor";
  }
}

TEST(CommandGetKeys, RepliesLikeRedis) {  // server.c:4988-5025
  CommandHarness h;
  EXPECT_EQ(h.Run({"COMMAND", "GETKEYS", "MSET", "a", "1", "b", "2"}), Arr({Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"COMMAND", "GETKEYS", "ZUNIONSTORE", "d", "2", "a", "b"}),
            Arr({Bulk("d"), Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"COMMAND", "GETKEYS", "NOPE"}), Err("ERR Invalid command specified"));
  EXPECT_EQ(h.Run({"COMMAND", "GETKEYS", "PING"}), Err("ERR The command has no key arguments"));
  EXPECT_EQ(h.Run({"COMMAND", "GETKEYS", "GET"}),
            Err("ERR Invalid number of arguments specified for command"));
  EXPECT_EQ(h.Run({"COMMAND", "GETKEYS", "EVAL", "return 1", "0"}), "*0\r\n");
  EXPECT_EQ(h.Run({"COMMAND", "GETKEYS", "ZUNIONSTORE", "d", "x", "a"}),
            Err("ERR Invalid arguments specified for command"));
}

TEST(CommandInfo, MovableKeysCommandsSayMovablekeys) {
  CommandHarness h;
  EXPECT_NE(h.Run({"COMMAND", "INFO", "zunionstore"}).find("+movablekeys\r\n"), std::string::npos);
  EXPECT_NE(h.Run({"COMMAND", "INFO", "eval"}).find("+movablekeys\r\n"), std::string::npos);
  EXPECT_EQ(h.Run({"COMMAND", "INFO", "get"}).find("movablekeys"), std::string::npos);
}

}  // namespace
}  // namespace tideskv::test
