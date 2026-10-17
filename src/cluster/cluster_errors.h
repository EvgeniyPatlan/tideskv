#pragma once
#include <string_view>

// Cluster reply texts (M7a). Client-facing routing and restriction texts are Redis 7.2's (cluster.c, db.c,
// replication.c, script.c); the CLUSTERX protocol and topology texts are Kvrocks v2.17.0's
// (cmd_cluster.cc, cluster.cc, cluster_defs.h, commander.cc), which kvrocks-controller and the ported gocase
// tests expect.
namespace tideskv::cluster {

// Redis 7.2.
inline constexpr std::string_view kErrCrossSlot = "CROSSSLOT Keys in request don't hash to the same slot";
inline constexpr std::string_view kErrSlotNotServed = "CLUSTERDOWN Hash slot not served";
inline constexpr std::string_view kErrClusterDisabled = "ERR This instance has cluster support disabled";
inline constexpr std::string_view kErrSelectInCluster = "ERR SELECT is not allowed in cluster mode";
inline constexpr std::string_view kErrReplicaofInCluster = "ERR REPLICAOF not allowed in cluster mode.";
inline constexpr std::string_view kScriptErrNonLocalKey =
    "Script attempted to access a non local key in a cluster node";
inline constexpr std::string_view kScriptErrCrossSlot =
    "Script attempted to access keys that do not hash to the same slot";
inline constexpr std::string_view kErrInvalidSlot = "ERR Invalid slot";  // COUNTKEYSINSLOT
inline constexpr std::string_view kErrInvalidSlotOrCount =
    "ERR Invalid slot or number of keys";  // GETKEYSINSLOT

// Kvrocks v2.17.0.
inline constexpr std::string_view kErrClusterNotEnabled = "ERR Cluster mode is not enabled";  // CLUSTERX
inline constexpr std::string_view kErrClusterNotInitialized = "CLUSTERDOWN The cluster is not initialized";
inline constexpr std::string_view kErrClusterxUsage =
    "ERR CLUSTERX command, CLUSTERX VERSION|MYID|SETNODEID|SETNODES|SETSLOT|MIGRATE|FLUSHSLOTS";
inline constexpr std::string_view kErrInvalidVersion = "ERR Invalid version";
inline constexpr std::string_view kErrInvalidSetnodesOptions = "ERR Invalid setnodes options";
inline constexpr std::string_view kErrInvalidClusterVersion = "ERR Invalid cluster version";
inline constexpr std::string_view kErrInvalidNodeId = "ERR Invalid cluster node id";
inline constexpr std::string_view kErrInvalidNodePort = "ERR Invalid cluster node port";
inline constexpr std::string_view kErrInvalidNodeRole = "ERR Invalid cluster node role";
inline constexpr std::string_view kErrInvalidNodesInfo = "ERR Invalid cluster nodes info";
inline constexpr std::string_view kErrSlotOutOfRange = "ERR Slot is out of range";
inline constexpr std::string_view kErrSlotOverlapped = "ERR Slot distribution is overlapped";
inline constexpr std::string_view kErrMissingNewline =
    "ERR Invalid nodes definition: Missing newline between node entries.";
inline constexpr std::string_view kErrInvalidSetslotOptions = "ERR Invalid setslot options";
inline constexpr std::string_view kErrSetslotNodeId = "ERR Invalid node id";
inline constexpr std::string_view kErrNoSuchNode = "ERR No this node in the cluster";
inline constexpr std::string_view kErrNotAMaster = "ERR The node isn't a master";
inline constexpr std::string_view kErrResetNotEmpty = "ERR Can't reset cluster while database is not empty";
inline constexpr std::string_view kErrClusterReplication = "ERR failed to set master-replica replication: ";
inline constexpr std::string_view kErrWrongArguments =
    "ERR wrong number of arguments";  // MIGRATE, FLUSHSLOTS
inline constexpr std::string_view kErrAsyncTimeout = "ERR Async migration does not support timeout";
inline constexpr std::string_view kErrInvalidSyncFlag = "ERR Invalid sync flag";
inline constexpr std::string_view kErrMigrateTimeout = "ERR timeout is not an integer or out of range";
inline constexpr std::string_view kErrMigrateTimeoutNegative = "ERR timeout is negative";
inline constexpr std::string_view kErrNoSlotsToParse = "ERR No slots to parse.";
// ParseSlotRanges (Kvrocks v2.17.0 commander.cc:119-165); composed in nodes_parser.cc.
inline constexpr std::string_view kErrInvalidSlotIdPrefix = "ERR Invalid slot id: ";
inline constexpr std::string_view kSlotIdNonInteger = "encounter non-integer characters";
inline constexpr std::string_view kSlotIdOutOfRange = "out of numeric range";
inline constexpr std::string_view kErrInvalidSlotRangePrefix =
    "ERR Invalid slot range: `";  // then <range> + "`. "
inline constexpr std::string_view kInvalidSlotRangeDashSuffix =
    "The character '-' can't appear in the first or last position.";
inline constexpr std::string_view kInvalidSlotRangeFormSuffix =
    "The slot range should be of the form `int1-int2`.";
inline constexpr std::string_view kInvalidSlotRangeOrderSuffix =
    "The slot range `int1-int2` needs to satisfy the condition (int1 <= int2).";
inline constexpr std::string_view kErrInvalidSlotsPrefix = "ERR Invalid slots: `";  // then <text>
inline constexpr std::string_view kErrInvalidSlotsSuffix =
    "`. No slots to parse. Please use spaces to separate slots.";
// Redis 7.2 redirection reply: "MOVED <slot> <host>:<port>" (cluster.c).
inline constexpr std::string_view kMovedPrefix = "MOVED ";
// Kvrocks v2.17.0 cluster.cc:925 (M7b D4, D6): Route's answer for a write to a fenced slot, and for an ASKING
// write on an importing slot (D6). Equal to tideskv::kErrTryAgain (types/errors.h; cluster/ may not include
// types/): static_assert in commands/cluster_routing.cc.
inline constexpr std::string_view kErrTryAgainFenced =
    "TRYAGAIN Can't write to slot being migrated which is in write forbidden phase";

// CLUSTER IMPORT (Kvrocks v2.17.0 cluster.cc:348-404, slot_import.cc:31-77, cluster_defs.h:37-43; M7b D14).
inline constexpr std::string_view kErrSlaveCantImport = "ERR Slave can't import slot";
inline constexpr std::string_view kErrImportOwnSlot = "ERR Can't import slot which belongs to me";
inline constexpr std::string_view kErrOneImportPrefix =
    "ERR only one importing job is allowed, current importing: ";  // then the range
inline constexpr std::string_view kErrImportMismatchPrefix = "ERR mismatch slot, importing slot(s): ";
inline constexpr std::string_view kImportMismatchMiddle = ", but got: ";  // Kvrocks prints -1 for no import
inline constexpr std::string_view kErrInvalidImportState = "ERR Invalid import state";
inline constexpr std::string_view kErrSlotRangeInvalid = "ERR Slot range is invalid";
// TidesKV-only (M7b D14): IMPORTDATA outside the import link; IMPORT 1 or IMPORTDATA of an import that is
// not in start; an IMPORT start that waited too long for this node's cleanup of an overlapping range.
inline constexpr std::string_view kErrNotImportLink = "ERR this connection is not importing slots";
inline constexpr std::string_view kErrImportNotRunning = "ERR the import of slot(s) is not in progress";
inline constexpr std::string_view kErrImportCleanupPending =
    "ERR the cleanup of an overlapping slot range is still running";

// TidesKV-only (M7a D7, spec section 5.4): the <reason> after kErrClusterReplication when the promotion
// failed for an internal (non-user) cause; Kvrocks appends its own status text, TidesKV exposes no internals.
inline constexpr std::string_view kReplicationReasonStorage = "storage error";

// TidesKV-only (M7a D11): MIGRATE and FLUSHSLOTS arrive with M7b.
inline constexpr std::string_view kErrMigrateNotSupported = "ERR slot migration is not supported yet";

}  // namespace tideskv::cluster
