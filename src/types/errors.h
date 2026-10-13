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
inline constexpr size_t kMaxStringSize = 512ULL * 1024 * 1024;  // proto-max-bulk-len

}  // namespace tideskv
