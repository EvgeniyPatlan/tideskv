#pragma once
#include <cstddef>
#include <string_view>

// Replication-only reply texts (redis72-replication §6; replication.c 7.2).
namespace tideskv {

inline constexpr std::string_view kErrSyncNotSupported = "ERR SYNC is not supported, use PSYNC";  // TidesKV
inline constexpr std::string_view kErrPsyncFailoverToMaster = "ERR PSYNC FAILOVER can't be sent to a master.";
inline constexpr std::string_view kErrReplconfUnrecognized =
    "ERR Unrecognized REPLCONF option: ";  // + option
// + "<n> bytes"
inline constexpr std::string_view kErrReplconfIpTooLong =
    "ERR REPLCONF ip-address provided by replica instance is too long: ";
inline constexpr size_t kReplconfMaxIpLength = 255;  // NET_HOST_STR_LEN - 1

}  // namespace tideskv
