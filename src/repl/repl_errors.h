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

// TidesKV-only (slotless key layout): REPLCONF key-layout <slotted|slotless>, and the PSYNC of a replica that
// sent none (it predates key layouts, so it is slotted and cannot adopt another one) to a slotless master.
inline constexpr std::string_view kErrReplconfKeyLayout =
    "ERR REPLCONF key-layout must be slotted or slotless";
inline constexpr std::string_view kErrPsyncLayoutUnknown =
    "ERR replica key layout slotted does not match this master's slotless layout (format 2); upgrade the "
    "replica, which then adopts the master's layout on a full resynchronisation";

// TidesKV-only: this node's key layout changed between its REPLCONF key-layout reply and the PSYNC (a full
// sync adopted another one); the replica reconnects and learns the new layout.
inline constexpr std::string_view kErrPsyncLayoutChanged =
    "ERR this node's key layout changed since REPLCONF key-layout; reconnect";

// TidesKV-only (G5): a PSYNC that would need a full resynchronisation while the node shuts down; no
// snapshot is streamed, so the replica keeps its data and retries elsewhere or later.
inline constexpr std::string_view kErrPsyncShuttingDown = "ERR PSYNC refused: this node is shutting down";

// TidesKV-only (M7b D19, R16): a replog read found its first wanted entry trimmed. Status text, read by
// ReplSender (the link drops) and the slot migrator (the job fails). + "<n> is not retained"
inline constexpr std::string_view kMsgReplogEntryPrefix = "replog entry ";
inline constexpr std::string_view kMsgReplogEntryNotRetained = " is not retained";

// TidesKV-only (M7b D3): Status texts of repl::RespConn, shared by the replica handshake (they reach the
// log and, through the migrator, a CLUSTER MIGRATE status). Peer-neutral: the peer is a master or a target.
inline constexpr std::string_view kMsgNoReplyWithinTimeout = "no reply within the timeout";
inline constexpr std::string_view kMsgReplyLineTooLong = "protocol error: reply line too long";
inline constexpr std::string_view kMsgUnreadableReplyPrefix =
    "protocol error: unreadable reply to ";  // + command
// TidesKV-only (M7b D3): the slot migrator's transport got a reply that is neither +... nor -...; reaches a
// CLUSTER MIGRATE status after a stage prefix.
inline constexpr std::string_view kMsgUnexpectedReplyPrefix =
    "protocol error: unexpected reply to ";  // + command

}  // namespace tideskv
