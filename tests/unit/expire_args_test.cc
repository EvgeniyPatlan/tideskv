#include "commands/expire_args.h"

#include <gtest/gtest.h>

#include <limits>

#include "commands/reply_util.h"

namespace tideskv {
namespace {

constexpr int64_t kNow = 1'700'000'000'000;
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

std::string ErrOf(const Result<int64_t>& r) { return r.ok() ? "<ok>" : r.status().message(); }

TEST(ExpireArgs, PositiveExpireUnits) {
  EXPECT_EQ(*ParsePositiveExpire("10", ExpireUnit::kSeconds, kNow, "set"), kNow + 10'000);
  EXPECT_EQ(*ParsePositiveExpire("10", ExpireUnit::kMillis, kNow, "set"), kNow + 10);
  EXPECT_EQ(*ParsePositiveExpire("10", ExpireUnit::kUnixSeconds, kNow, "set"), 10'000);
  EXPECT_EQ(*ParsePositiveExpire("10", ExpireUnit::kUnixMillis, kNow, "set"), 10);
}

TEST(ExpireArgs, PositiveExpireRejectsNonPositiveAndGarbage) {  // Review Focus #3
  EXPECT_EQ(ErrOf(ParsePositiveExpire("0", ExpireUnit::kSeconds, kNow, "set")),
            "ERR invalid expire time in 'set' command");
  EXPECT_EQ(ErrOf(ParsePositiveExpire("-5", ExpireUnit::kMillis, kNow, "getex")),
            "ERR invalid expire time in 'getex' command");
  EXPECT_EQ(ErrOf(ParsePositiveExpire("1234xyz", ExpireUnit::kSeconds, kNow, "set")), kErrNotInteger);
  EXPECT_EQ(ErrOf(ParsePositiveExpire("", ExpireUnit::kSeconds, kNow, "set")), kErrNotInteger);
  EXPECT_EQ(ErrOf(ParsePositiveExpire("010", ExpireUnit::kSeconds, kNow, "set")), kErrNotInteger);
}

TEST(ExpireArgs, PositiveExpireOverflow) {  // Review Focus #3
  EXPECT_EQ(ErrOf(ParsePositiveExpire("9223372036854775807", ExpireUnit::kSeconds, kNow, "set")),
            "ERR invalid expire time in 'set' command");
  EXPECT_EQ(ErrOf(ParsePositiveExpire(std::to_string(kMax - kNow + 1), ExpireUnit::kMillis, kNow, "set")),
            "ERR invalid expire time in 'set' command");
  EXPECT_EQ(*ParsePositiveExpire(std::to_string(kMax - kNow), ExpireUnit::kMillis, kNow, "set"), kMax);
  EXPECT_EQ(*ParsePositiveExpire("9223372036854775807", ExpireUnit::kUnixMillis, kNow, "set"), kMax);
}

TEST(ExpireArgs, ExpireCommandAllowsNonPositive) {
  EXPECT_EQ(*ParseExpireCommandTime("-10", ExpireUnit::kSeconds, kNow, "expire"), kNow - 10'000);
  EXPECT_EQ(*ParseExpireCommandTime("0", ExpireUnit::kUnixSeconds, kNow, "expireat"), 0);
  EXPECT_EQ(*ParseExpireCommandTime("-1", ExpireUnit::kUnixMillis, kNow, "pexpireat"), -1);
}

TEST(ExpireArgs, ExpireCommandOverflow) {  // Review Focus #3
  EXPECT_EQ(ErrOf(ParseExpireCommandTime("9223372036854775807", ExpireUnit::kSeconds, kNow, "expire")),
            "ERR invalid expire time in 'expire' command");
  EXPECT_EQ(ErrOf(ParseExpireCommandTime("-9223372036854775807", ExpireUnit::kSeconds, kNow, "expire")),
            "ERR invalid expire time in 'expire' command");
  EXPECT_EQ(ErrOf(ParseExpireCommandTime("9223372036854775807", ExpireUnit::kMillis, kNow, "pexpire")),
            "ERR invalid expire time in 'pexpire' command");
  EXPECT_EQ(*ParseExpireCommandTime("9223372036854775807", ExpireUnit::kUnixMillis, kNow, "pexpireat"), kMax);
  EXPECT_EQ(ErrOf(ParseExpireCommandTime("abc", ExpireUnit::kSeconds, kNow, "expire")), kErrNotInteger);
}

ArgList Args(std::initializer_list<const char*> a) { return ArgList(a.begin(), a.end()); }

TEST(ExtendedArgs, SetOptionsAreCaseInsensitiveAndCombine) {
  const ArgList argv = Args({"set", "k", "v", "nx", "Get", "PX", "100"});  // expire_arg views into argv
  auto a = ParseExtendedArgs(argv, 3, ExtendedArgsKind::kSet);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->condition, SetOptions::Condition::kIfMissing);
  EXPECT_TRUE(a->get);
  EXPECT_EQ(a->unit, ExpireUnit::kMillis);
  EXPECT_EQ(a->expire_arg, "100");
}

TEST(ExtendedArgs, SameExpireOptionRepeatedLastWins) {
  const ArgList argv =
      Args({"set", "k", "v", "ex", "1", "ex", "2", "EX", "10"});  // expire_arg views into argv
  auto a = ParseExtendedArgs(argv, 3, ExtendedArgsKind::kSet);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->expire_arg, "10");
  const ArgList argv_b = Args({"set", "k", "v", "nx", "nx"});
  auto b = ParseExtendedArgs(argv_b, 3, ExtendedArgsKind::kSet);
  EXPECT_TRUE(b.ok());
}

TEST(ExtendedArgs, SyntaxErrors) {
  for (auto argv :
       {Args({"set", "k", "v", "nx", "xx"}), Args({"set", "k", "v", "xx", "nx"}),
        Args({"set", "k", "v", "px", "100", "ex", "10"}), Args({"set", "k", "v", "exat", "1", "pxat", "1"}),
        Args({"set", "k", "v", "keepttl", "ex", "100"}), Args({"set", "k", "v", "ex", "100", "keepttl"}),
        Args({"set", "k", "v", "ex"}), Args({"set", "k", "v", "persist"}),
        Args({"set", "k", "v", "non-existing-option"})}) {
    auto a = ParseExtendedArgs(argv, 3, ExtendedArgsKind::kSet);
    ASSERT_FALSE(a.ok()) << argv.back();
    EXPECT_EQ(a.status().message(), kErrSyntax);
  }
}

TEST(ExtendedArgs, GetExAcceptsPersistButNotSetOptions) {
  const ArgList persist_argv = Args({"getex", "k", "persist"});
  auto a = ParseExtendedArgs(persist_argv, 2, ExtendedArgsKind::kGetEx);
  ASSERT_TRUE(a.ok());
  EXPECT_TRUE(a->persist);
  for (auto argv :
       {Args({"getex", "k", "nx"}), Args({"getex", "k", "get"}), Args({"getex", "k", "keepttl"}),
        Args({"getex", "k", "ex", "10", "persist"}), Args({"getex", "k", "persist", "px", "1"})}) {
    EXPECT_FALSE(ParseExtendedArgs(argv, 2, ExtendedArgsKind::kGetEx).ok()) << argv.back();
  }
}

}  // namespace
}  // namespace tideskv
