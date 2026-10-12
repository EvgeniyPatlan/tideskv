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
inline constexpr std::string_view kErrRenameCrossSlot =
    "ERR RENAME of a collection across hash slots is not supported yet";  // M2 limitation; M3 copies
inline constexpr size_t kMaxStringSize = 512ULL * 1024 * 1024;            // proto-max-bulk-len

}  // namespace tideskv
