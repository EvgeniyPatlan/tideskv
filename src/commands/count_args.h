#pragma once
#include <cstdint>
#include <string_view>

#include "common/status.h"

// Redis 7.2 object.c count/limit argument rules shared by the collection commands.
namespace tideskv {

// HRANDFIELD/SRANDMEMBER count: getRangeLongFromObjectOrReply(-LONG_MAX, LONG_MAX, NULL).
Status ParseRandomCount(std::string_view arg, int64_t* out);
// getPositiveLongFromObjectOrReply(msg): a parse error and a negative value both reply `error`
// (SPOP count: kErrMustBePositive; SINTERCARD LIMIT: kErrLimitNegative).
Status ParsePositiveCount(std::string_view arg, std::string_view error, int64_t* out);

// getPositiveLongFromObjectOrReply(NULL) (LPOP/RPOP count): getRangeLong replies the default
// message kErrMustBePositive for a non-integer and for a negative value alike.
Status ParseNonNegativeCount(std::string_view arg, int64_t* out);

}  // namespace tideskv
