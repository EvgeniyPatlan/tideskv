#include "commands/expire_args.h"

#include <limits>

#include "commands/reply_util.h"
#include "common/string_util.h"

namespace tideskv {
namespace {

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

bool IsSeconds(ExpireUnit u) { return u == ExpireUnit::kSeconds || u == ExpireUnit::kUnixSeconds; }
bool IsRelative(ExpireUnit u) { return u == ExpireUnit::kSeconds || u == ExpireUnit::kMillis; }

std::optional<ExpireUnit> UnitOf(std::string_view opt) {
  if (EqualsIgnoreCase(opt, "EX")) return ExpireUnit::kSeconds;
  if (EqualsIgnoreCase(opt, "PX")) return ExpireUnit::kMillis;
  if (EqualsIgnoreCase(opt, "EXAT")) return ExpireUnit::kUnixSeconds;
  if (EqualsIgnoreCase(opt, "PXAT")) return ExpireUnit::kUnixMillis;
  return std::nullopt;
}

Status NotInteger() { return Status::UserError(std::string(kErrNotInteger)); }

}  // namespace

Result<int64_t> ParsePositiveExpire(std::string_view arg, ExpireUnit unit, int64_t now_ms,
                                    std::string_view command) {
  int64_t v = 0;
  if (!ParseInt64(arg, &v)) return NotInteger();
  if (v <= 0 || (IsSeconds(unit) && v > kMax / 1000)) return Status::UserError(InvalidExpireError(command));
  if (IsSeconds(unit)) v *= 1000;
  if (IsRelative(unit)) {
    if (v > kMax - now_ms) return Status::UserError(InvalidExpireError(command));
    v += now_ms;
  }
  return v;
}

Result<int64_t> ParseExpireCommandTime(std::string_view arg, ExpireUnit unit, int64_t now_ms,
                                       std::string_view command) {
  int64_t v = 0;
  if (!ParseInt64(arg, &v)) return NotInteger();
  if (IsSeconds(unit)) {
    if (v > kMax / 1000 || v < kMin / 1000) return Status::UserError(InvalidExpireError(command));
    v *= 1000;
  }
  const int64_t base = IsRelative(unit) ? now_ms : 0;
  if (v > kMax - base) return Status::UserError(InvalidExpireError(command));
  return v + base;
}

Result<ExtendedArgs> ParseExtendedArgs(const ArgList& argv, size_t first, ExtendedArgsKind kind) {
  using Condition = SetOptions::Condition;
  ExtendedArgs a;
  const bool set = kind == ExtendedArgsKind::kSet;
  for (size_t j = first; j < argv.size(); ++j) {
    const std::string& opt = argv[j];
    const bool has_next = j + 1 < argv.size();
    const std::optional<ExpireUnit> unit = UnitOf(opt);
    if (set && EqualsIgnoreCase(opt, "NX") && a.condition != Condition::kIfExists) {
      a.condition = Condition::kIfMissing;
    } else if (set && EqualsIgnoreCase(opt, "XX") && a.condition != Condition::kIfMissing) {
      a.condition = Condition::kIfExists;
    } else if (set && EqualsIgnoreCase(opt, "GET")) {
      a.get = true;
    } else if (set && EqualsIgnoreCase(opt, "KEEPTTL") && !a.unit) {
      a.keep_ttl = true;
    } else if (!set && EqualsIgnoreCase(opt, "PERSIST") && !a.unit) {
      a.persist = true;
    } else if (unit && has_next && !a.keep_ttl && !a.persist && (!a.unit || *a.unit == *unit)) {
      a.unit = unit;
      a.expire_arg = argv[++j];
    } else {
      return Status::UserError(std::string(kErrSyntax));
    }
  }
  return a;
}

}  // namespace tideskv
