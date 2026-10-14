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
inline constexpr std::string_view kErrMultiNested = "ERR MULTI calls can not be nested";
inline constexpr std::string_view kErrExecNoMulti = "ERR EXEC without MULTI";
inline constexpr std::string_view kErrDiscardNoMulti = "ERR DISCARD without MULTI";
inline constexpr std::string_view kErrNotAllowedInMulti = "ERR Command not allowed inside a transaction";
inline constexpr std::string_view kErrExecAbort =
    "EXECABORT Transaction discarded because of previous errors.";
inline constexpr std::string_view kErrExecAbortBecause = "EXECABORT Transaction discarded because of: ";
inline constexpr std::string_view kErrNoAuth = "NOAUTH Authentication required.";

// Scripting (M5b; eval.c, script_lua.c, script.c 7.2).
inline constexpr std::string_view kErrNoScript = "NOSCRIPT No matching script. Please use EVAL.";
inline constexpr std::string_view kErrNumkeysNegative = "ERR Number of keys can't be negative";
inline constexpr std::string_view kErrNumkeysTooBig =
    "ERR Number of keys can't be greater than number of args";
inline constexpr std::string_view kErrRoScriptWithWriteFlag =
    "ERR Can not execute a script with write flag using *_ro command.";
inline constexpr std::string_view kErrBusyScript =
    "BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.";
inline constexpr std::string_view kErrNotBusy = "NOTBUSY No scripts in execution right now.";
inline constexpr std::string_view kErrReadOnlyReplica =
    "READONLY You can't write against a read only replica.";
inline constexpr std::string_view kErrScriptFlushOption = "ERR SCRIPT FLUSH only support SYNC|ASYNC option";
inline constexpr std::string_view kErrScriptDebugOption = "ERR Use SCRIPT DEBUG YES/SYNC/NO";
inline constexpr std::string_view kErrScriptDebugUnsupported = "ERR SCRIPT DEBUG YES/SYNC is not supported";
inline constexpr std::string_view kErrInvalidShebang = "ERR Invalid script shebang";
inline constexpr std::string_view kErrInvalidShebangEngine = "ERR Invalid engine in script shebang";
inline constexpr std::string_view kErrUnexpectedShebangEngine = "ERR Unexpected engine in script shebang: ";
inline constexpr std::string_view kErrUnknownShebangOption = "ERR Unknown lua shebang option: ";
inline constexpr std::string_view kErrUnexpectedShebangFlag = "ERR Unexpected flag in script shebang: ";
inline constexpr std::string_view kErrCompilingScript = "ERR Error compiling script (new function): ";
// Raised inside scripts (no code: luaPushErrorBuff prefixes "ERR ").
inline constexpr std::string_view kScriptErrUnknownCommand = "Unknown Redis command called from script";
inline constexpr std::string_view kScriptErrArity = "Wrong number of args calling Redis command from script";
inline constexpr std::string_view kScriptErrNoScript = "This Redis command is not allowed from script";
inline constexpr std::string_view kScriptErrWriteInRo =
    "Write commands are not allowed from read-only scripts.";
inline constexpr std::string_view kScriptErrNoArgs =
    "Please specify at least one argument for this redis lib call";
inline constexpr std::string_view kScriptErrArgType =
    "Lua redis lib command arguments must be strings or integers";
// Internal (not a reply text): the compiled script could not be stored in the Lua registry.
inline constexpr std::string_view kErrLuaStoreScript = "cannot store the compiled script";
inline constexpr std::string_view kScriptErrKilled = "Script killed by user with SCRIPT KILL...";
inline constexpr std::string_view kScriptErrWrongArgs = "wrong number of arguments";
inline constexpr std::string_view kScriptErrWrongArgsOrType = "wrong number or type of arguments";
inline constexpr std::string_view kScriptErrNonexistentGlobal =
    "Script attempted to access nonexistent global variable '";
inline constexpr std::string_view kScriptErrLogArgs = "redis.log() requires two arguments or more.";
inline constexpr std::string_view kScriptErrLogLevelType = "First argument must be a number (log level).";
inline constexpr std::string_view kScriptErrLogLevel = "Invalid debug level.";
inline constexpr std::string_view kScriptErrSetRespArgs = "redis.setresp() requires one argument.";
inline constexpr std::string_view kScriptErrSetRespVersion = "RESP version must be 2 or 3.";
inline constexpr std::string_view kScriptErrSetReplArgs = "redis.set_repl() requires one argument.";
inline constexpr std::string_view kScriptErrSetReplFlags =
    "Invalid replication flags. Use REPL_AOF, REPL_REPLICA, REPL_ALL or REPL_NONE.";
inline constexpr std::string_view kScriptErrAclCheckCmd = "Invalid command passed to redis.acl_check_cmd()";
// Raised by the protection metamethods when called directly (sl:1267-1350).
inline constexpr std::string_view kScriptErrProtectedTableArgs =
    "Wrong number of arguments to luaProtectedTableError";
inline constexpr std::string_view kScriptErrProtectedTableKey =
    "Second argument to luaProtectedTableError must be a string or number";
inline constexpr std::string_view kScriptErrAllowListArgs =
    "Wrong number of arguments to luaNewIndexAllowList";
inline constexpr std::string_view kScriptErrAllowListTable =
    "first argument to luaNewIndexAllowList must be a table";
inline constexpr std::string_view kScriptErrAllowListKey =
    "Second argument to luaNewIndexAllowList must be a string or number";

}  // namespace tideskv
