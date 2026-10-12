#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

#include "commands/registry.h"
#include "common/status.h"
#include "types/string_type.h"

// Expire arithmetic and SET/GETEX option parsing, matching Redis 7.2 exactly.
namespace tideskv {

enum class ExpireUnit : uint8_t { kSeconds, kMillis, kUnixSeconds, kUnixMillis };

// SET/SETEX/PSETEX/GETEX: the argument must be a positive integer
// (getExpireMillisecondsOrReply). Returns the absolute deadline in unix ms.
Result<int64_t> ParsePositiveExpire(std::string_view arg, ExpireUnit unit, int64_t now_ms,
                                    std::string_view command);
// EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT: any integer (expireGenericCommand). The deadline
// may be <= now_ms, which means "delete the key".
Result<int64_t> ParseExpireCommandTime(std::string_view arg, ExpireUnit unit, int64_t now_ms,
                                       std::string_view command);

enum class ExtendedArgsKind : uint8_t { kSet, kGetEx };

struct ExtendedArgs {
  SetOptions::Condition condition = SetOptions::Condition::kAlways;
  bool get = false;
  bool keep_ttl = false;
  bool persist = false;
  std::optional<ExpireUnit> unit;  // set when EX/PX/EXAT/PXAT was given
  std::string_view expire_arg;     // the last one given; views into argv
};

// Parses argv[first..] like Redis 7.2 parseExtendedStringArgumentsOrReply.
Result<ExtendedArgs> ParseExtendedArgs(const ArgList& argv, size_t first, ExtendedArgsKind kind);

}  // namespace tideskv
