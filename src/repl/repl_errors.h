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
// REPLICAOF (replicaofCommand, redis72-replication §1.2).
inline constexpr std::string_view kErrInvalidMasterPort = "ERR Invalid master port";
inline constexpr std::string_view kErrReplicaofFromReplica =
    "ERR Command is not valid when client is a replica.";
inline constexpr std::string_view kReplyAlreadyConnected =
    "OK Already connected to specified master";  // status

// TidesKV-only (G5): a PSYNC that would need a full resynchronisation while the node shuts down; no
// snapshot is streamed, so the replica keeps its data and retries elsewhere or later.
inline constexpr std::string_view kErrPsyncShuttingDown = "ERR PSYNC refused: this node is shutting down";

// TidesKV-only (M7b D19, R16): a replog read found its first wanted entry trimmed. Status text, read by
// ReplSender (the link drops) and the slot migrator (the job fails). + "<n> is not retained"
inline constexpr std::string_view kMsgReplogEntryPrefix = "replog entry ";
inline constexpr std::string_view kMsgReplogEntryNotRetained = " is not retained";

}  // namespace tideskv
