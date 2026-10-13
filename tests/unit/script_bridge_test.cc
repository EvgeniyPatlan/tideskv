#include "commands/script_bridge.h"

#include <gtest/gtest.h>

#include "support/command_harness.h"

namespace tideskv::test {
namespace {

std::string Call(ScriptBridge& b, const ArgList& argv, int resp = 2) {
  std::string err;
  auto r = b.Call(argv, resp, &err);
  return r ? *r : "REJECT:" + err;
}

TEST(ScriptBridge, RunsCommandsInTheCallersDbWithoutChangingIt) {
  CommandHarness h;
  h.Run({"SELECT", "3"});
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  EXPECT_EQ(Call(b, {"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(Call(b, {"SELECT", "5"}), "+OK\r\n");
  EXPECT_EQ(Call(b, {"GET", "k"}), "$-1\r\n");  // db 5 now, inside the script only
  EXPECT_EQ(h.session.db, 3);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
}

TEST(ScriptBridge, TheScriptsProtocolNeverLeaksToTheCaller) {
  CommandHarness h;
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  EXPECT_EQ(Call(b, {"HSET", "h", "f", "v"}, 3), ":1\r\n");
  EXPECT_EQ(h.session.protocol, 2);
}

TEST(ScriptBridge, RejectsLikeRedis) {  // scriptCall, script.c:513-573
  CommandHarness h;
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  EXPECT_EQ(Call(b, {"NOSUCH"}), "REJECT:Unknown Redis command called from script");
  EXPECT_EQ(Call(b, {"GET"}), "REJECT:Wrong number of args calling Redis command from script");
  EXPECT_EQ(Call(b, {"MULTI"}), "REJECT:This Redis command is not allowed from script");
  EXPECT_EQ(Call(b, {"QUIT"}), "REJECT:This Redis command is not allowed from script");
  b.SetReadOnly(true);
  EXPECT_EQ(Call(b, {"SET", "k", "v"}), "REJECT:Write commands are not allowed from read-only scripts.");
  EXPECT_EQ(Call(b, {"GET", "k"}), "$-1\r\n");
}

TEST(ScriptBridge, BlockingCommandsAnswerAtOnceAndCommandErrorsAreReplies) {
  CommandHarness h;
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  EXPECT_EQ(Call(b, {"BLPOP", "q", "0"}), "*-1\r\n");
  EXPECT_EQ(Call(b, {"BLMOVE", "q", "d", "LEFT", "LEFT", "0"}), "$-1\r\n");
  Call(b, {"SET", "s", "x"});
  EXPECT_EQ(Call(b, {"LPUSH", "s", "x"}).substr(0, 10), "-WRONGTYPE");
  EXPECT_EQ(Call(b, {"HSET", "h", "f", "v"}, 3), ":1\r\n");
  EXPECT_EQ(Call(b, {"HGETALL", "h"}, 3), "%1\r\n$1\r\nf\r\n$1\r\nv\r\n");
}

TEST(ScriptBridge, PushesSignalWaitersThroughTheCallersReadyList) {
  CommandHarness h;
  SecondClient w(h);
  EXPECT_EQ(w.Run({"BLPOP", "q", "0"}), "");
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  Call(b, {"LPUSH", "q", "x"});
  EXPECT_FALSE(h.ready.empty());  // served by the caller after the script
}

}  // namespace
}  // namespace tideskv::test
