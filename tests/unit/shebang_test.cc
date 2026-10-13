#include "script/shebang.h"

#include <gtest/gtest.h>

namespace tideskv {
namespace {

std::string Err(std::string_view body) {
  auto r = ParseShebang(body);
  return r.ok() ? "<ok>" : r.status().message();
}

TEST(Shebang, NoShebangIsCompatMode) {
  auto r = ParseShebang("return 1");
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->flags.shebang);
  EXPECT_EQ(r->code, "return 1");
}

TEST(Shebang, FlagsAreParsedAndTheLineIsStrippedKeepingItsNewline) {
  auto r = ParseShebang("#!lua flags=no-writes,allow-oom\nreturn 1");
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->flags.shebang);
  EXPECT_TRUE(r->flags.no_writes);
  EXPECT_TRUE(r->flags.allow_oom);
  EXPECT_FALSE(r->flags.allow_stale);
  EXPECT_EQ(r->code, "\nreturn 1");  // line numbers stay right (eval.c:443)
}

TEST(Shebang, EmptyFlagsAndPlainLuaAreAccepted) {
  EXPECT_EQ(Err("#!lua flags=\nreturn 1"), "<ok>");
  EXPECT_EQ(Err("#!lua\nreturn 1"), "<ok>");
  EXPECT_EQ(Err("#!lua flags=allow-stale,no-cluster,allow-cross-slot-keys\nreturn 1"), "<ok>");
}

TEST(Shebang, ErrorsMatchRedis) {  // eval.c:315-378, tst:1719-1748
  EXPECT_EQ(Err("#!lua flags=no-writes"), "ERR Invalid script shebang");
  EXPECT_EQ(Err("#!not-lua\nreturn 1"), "ERR Unexpected engine in script shebang: #!not-lua");
  EXPECT_EQ(Err("#!lua foo=bar\nreturn 1"), "ERR Unknown lua shebang option: foo=bar");
  EXPECT_EQ(Err("#!lua flags=allow-oom,what?\nreturn 1"), "ERR Unexpected flag in script shebang: what?");
  EXPECT_EQ(Err("#!lua flags=a,,b\nreturn 1"), "ERR Unexpected flag in script shebang: a");
  EXPECT_EQ(Err("#!lua flags=no-writes,,allow-oom\nreturn 1"), "ERR Unexpected flag in script shebang: ");
}

}  // namespace
}  // namespace tideskv
