#pragma once
#include <cstddef>
#include <string_view>

// Exact Redis 7.2 error reply texts and limits shared by the type layer and the commands.
namespace tideskv {

inline constexpr std::string_view kErrWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
inline constexpr std::string_view kErrNotInteger = "ERR value is not an integer or out of range";
inline constexpr std::string_view kErrNotFloat = "ERR value is not a valid float";
inline constexpr std::string_view kErrSyntax = "ERR syntax error";
inline constexpr std::string_view kErrNoSuchKey = "ERR no such key";
inline constexpr std::string_view kErrOverflow = "ERR increment or decrement would overflow";
inline constexpr std::string_view kErrNanOrInfinity = "ERR increment would produce NaN or Infinity";
inline constexpr std::string_view kErrStringTooLong =
    "ERR string exceeds maximum allowed size (proto-max-bulk-len)";
inline constexpr std::string_view kErrOffsetOutOfRange = "ERR offset is out of range";
inline constexpr std::string_view kErrDbIndexOutOfRange = "ERR DB index is out of range";
inline constexpr std::string_view kErrInvalidCursor = "ERR invalid cursor";
inline constexpr std::string_view kErrDecrementOverflow = "ERR decrement would overflow";
inline constexpr std::string_view kErrLcsNotStrings = "ERR The specified keys must contain string values";
inline constexpr std::string_view kErrLcsLenAndIdx =
    "ERR If you want both the length and indexes, please just use IDX.";
inline constexpr std::string_view kErrLcsMemory =
    "ERR Insufficient memory, transient memory for LCS exceeds proto-max-bulk-len";
inline constexpr std::string_view kErrExpireNxCombo =
    "ERR NX and XX, GT or LT options at the same time are not compatible";
inline constexpr std::string_view kErrExpireGtLt =
    "ERR GT and LT options at the same time are not compatible";
// M3 (Redis 7.2.5 t_hash.c / t_set.c / object.c).
inline constexpr std::string_view kErrHashValueNotInteger = "ERR hash value is not an integer";
inline constexpr std::string_view kErrHashValueNotFloat = "ERR hash value is not a float";
inline constexpr std::string_view kErrValueNanOrInfinity = "ERR value is NaN or Infinity";
inline constexpr std::string_view kErrValueOutOfRange = "ERR value is out of range";
// getRangeLongFromObjectOrReply(-LONG_MAX, LONG_MAX) with no message (HRANDFIELD/SRANDMEMBER count).
inline constexpr std::string_view kErrCountOutOfRange =
    "ERR value is out of range, value must between -9223372036854775807 and 9223372036854775807";
// getPositiveLongFromObjectOrReply with no message (SPOP count): used for parse errors too.
inline constexpr std::string_view kErrMustBePositive = "ERR value is out of range, must be positive";
inline constexpr std::string_view kErrNumkeysNotPositive = "ERR numkeys should be greater than 0";
inline constexpr std::string_view kErrNumkeysTooMany =
    "ERR Number of keys can't be greater than number of args";
inline constexpr std::string_view kErrLimitNegative = "ERR LIMIT can't be negative";
// M4 (Redis 7.2.5 t_list.c, t_zset.c, blocked.c, timeout.c; see notes/m4-research).
inline constexpr std::string_view kErrIndexOutOfRange = "ERR index out of range";
inline constexpr std::string_view kErrRankZero =
    "ERR RANK can't be zero: use 1 to start from the first match, 2 from the second ... or use negative to "
    "start "
    "from the end of the list";
inline constexpr std::string_view kErrCountNegative = "ERR COUNT can't be negative";
inline constexpr std::string_view kErrMaxlenNegative = "ERR MAXLEN can't be negative";
inline constexpr std::string_view kErrCountGreaterThanZero = "ERR count should be greater than 0";
inline constexpr size_t kMaxStringSize = 512ULL * 1024 * 1024;  // proto-max-bulk-len

// getTimeoutFromObjectOrReply(UNIT_SECONDS) (timeout.c:165).
inline constexpr std::string_view kErrTimeoutNotFloat = "ERR timeout is not a float or out of range";
inline constexpr std::string_view kErrTimeoutNegative = "ERR timeout is negative";
inline constexpr std::string_view kErrTimeoutOutOfRange = "ERR timeout is out of range";

inline constexpr std::string_view kErrUnblockReason = "ERR CLIENT UNBLOCK reason should be TIMEOUT or ERROR";
inline constexpr std::string_view kErrUnblockedByClient = "UNBLOCKED client unblocked via CLIENT UNBLOCK";
// TidesKV-only: an exception escaped a command (the connection's catch-all, a blocked command's re-run).
inline constexpr std::string_view kErrInternal = "ERR internal error";

// M4 sorted sets (zslParseRange, zslParseLexRange, ZADD/ZINCRBY NaN, ZUNION WEIGHTS).
inline constexpr std::string_view kErrMinMaxNotFloat = "ERR min or max is not a float";
inline constexpr std::string_view kErrMinMaxNotString = "ERR min or max not valid string range item";
inline constexpr std::string_view kErrScoreNan = "ERR resulting score is not a number (NaN)";
inline constexpr std::string_view kErrWeightNotFloat = "ERR weight value is not a float";

inline constexpr std::string_view kErrZaddXxNx = "ERR XX and NX options at the same time are not compatible";
inline constexpr std::string_view kErrZaddGtLtNx =
    "ERR GT, LT, and/or NX options at the same time are not compatible";
inline constexpr std::string_view kErrZaddIncrPair =
    "ERR INCR option supports a single increment-element pair";

inline constexpr std::string_view kErrZrangeLimit =
    "ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX";
inline constexpr std::string_view kErrZrangeWithscoresLex =
    "ERR syntax error, WITHSCORES not supported in combination with BYLEX";

// Transactions (M5a; multi.c, server.c 7.2.5).
inline constexpr std::string_view kErrWatchInMulti = "ERR WATCH inside MULTI is not allowed";

}  // namespace tideskv
