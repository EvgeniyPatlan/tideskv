#include <gtest/gtest.h>

#include <string>

#include "commands/script_bridge.h"
#include "script/sha1hex.h"
#include "support/cluster_harness.h"

namespace tideskv::test {
namespace {

const std::string kNonLocal = "Script attempted to access a non local key in a cluster node";
const std::string kScriptCross = "Script attempted to access keys that do not hash to the same slot";

struct RoutedA : CommandHarness {
  RoutedA() : cluster(*this) { cluster.Push(kThreeNodes, 1); }
  ClusterHarness cluster;
};

std::string Call(ScriptBridge& b, const ArgList& argv) {
  std::string err;
  auto r = b.Call(argv, 2, &err);
  return r ? *r : "REJECT:" + err;
}

std::string Suffix(const std::string& body, int line) {
  return " script: " + Sha1Hex(body) + ", on @user_script:" + std::to_string(line) + ".";
}

TEST(ClusterScript, ACallToAForeignKeyIsANonLocalKeyError) {  // script.c:420-460
  RoutedA h;
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  EXPECT_EQ(Call(b, {"GET", "foo"}), "REJECT:" + kNonLocal);
  EXPECT_EQ(Call(b, {"GET", "b"}), kNil);
}

TEST(ClusterScript, CompatScriptsMayTouchSeveralLocalSlots) {  // EVAL_COMPAT_MODE allows cross-slot
  RoutedA h;
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  EXPECT_EQ(Call(b, {"SET", "b", "1"}), kOk);
  EXPECT_EQ(Call(b, {"SET", "bar", "1"}), kOk);
}

TEST(ClusterScript, OtherShebangScriptsStayInTheirDeclaredSlot) {
  RoutedA h;
  ExecContext ctx{h.session, h.reply, h.services, nullptr, &h.ready};
  ScriptBridge b(ctx);
  b.SetCrossSlotAllowed(false);
  b.SetDeclaredKeys({"b"});
  EXPECT_EQ(Call(b, {"SET", "{b}x", "1"}), kOk);
  EXPECT_EQ(Call(b, {"SET", "bar", "1"}), "REJECT:" + kScriptCross);
}

TEST(ClusterScript, EvalRepliesCarryTheScriptSuffix) {  // unit/cluster/scripting.tcl
  RoutedA h;
  const std::string foreign = "return redis.call('get','foo')";
  EXPECT_EQ(h.Run({"EVAL", foreign, "0"}), Err("ERR " + kNonLocal + Suffix(foreign, 1)));
  const std::string cross = "#!lua\nredis.call('set','b','1')\nreturn redis.call('set','bar','1')";
  EXPECT_EQ(h.Run({"EVAL", cross, "0"}), Err("ERR " + kScriptCross + Suffix(cross, 3)));
  const std::string allowed =
      "#!lua flags=allow-cross-slot-keys\nredis.call('set','b','1')\nreturn redis.call('set','bar','1')";
  EXPECT_EQ(h.Run({"EVAL", allowed, "0"}), kOk);
}

TEST(ClusterScript, NoClusterScriptsAreRefusedOnlyInClusterMode) {  // script.c:141-143
  const std::string body = "#!lua flags=no-cluster\nreturn 1";
  RoutedA h;
  EXPECT_EQ(h.Run({"EVAL", body, "0"}), Err("ERR Can not run script on cluster, 'no-cluster' flag is set."));
  CommandHarness plain;
  EXPECT_EQ(plain.Run({"EVAL", body, "0"}), Int(1));
}

TEST(ClusterScript, TheCallersReadonlyFlagCoversTheScriptsCalls) {  // script.c:428-430
  CommandHarness h;
  ClusterHarness c(h, 7002);  // A's replica
  c.Push(kThreeNodes, 1);
  const std::string body = "#!lua flags=no-writes\nreturn redis.call('get','b')";
  EXPECT_EQ(h.Run({"EVAL", body, "0"}), Err("ERR " + kNonLocal + Suffix(body, 2)));
  ASSERT_EQ(h.Run({"READONLY"}), kOk);
  EXPECT_EQ(h.Run({"EVAL", body, "0"}), kNil);
}

}  // namespace
}  // namespace tideskv::test
