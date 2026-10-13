#include "commands/numkeys_args.h"

#include <string>

#include "common/string_util.h"
#include "types/errors.h"

namespace tideskv {
namespace {

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

}  // namespace

Status ParseMultiPop(const ArgList& argv, size_t numkeys_idx, std::string_view first_word,
                     std::string_view second_word, MultiPopArgs* out) {
  int64_t numkeys = 0;
  if (!ParseInt64(argv[numkeys_idx], &numkeys) || numkeys < 1) return Err(kErrNumkeysNotPositive);
  // The direction word must exist after the keys: numkeys <= argc - numkeys_idx - 2 (no overflow).
  const size_t room = argv.size() - numkeys_idx - 1;  // arguments after numkeys
  if (static_cast<uint64_t>(numkeys) >= room) return Err(kErrSyntax);
  MultiPopArgs a;
  a.first_key = numkeys_idx + 1;
  a.num_keys = static_cast<size_t>(numkeys);
  const size_t where = a.first_key + a.num_keys;
  if (EqualsIgnoreCase(argv[where], first_word)) {
    a.second = false;
  } else if (EqualsIgnoreCase(argv[where], second_word)) {
    a.second = true;
  } else {
    return Err(kErrSyntax);
  }
  for (size_t j = where + 1; j < argv.size(); j += 2) {
    if (a.has_count || !EqualsIgnoreCase(argv[j], "COUNT") || j + 1 >= argv.size()) return Err(kErrSyntax);
    if (!ParseInt64(argv[j + 1], &a.count) || a.count < 1) return Err(kErrCountGreaterThanZero);
    a.has_count = true;
  }
  *out = a;
  return Status::Ok();
}

}  // namespace tideskv
