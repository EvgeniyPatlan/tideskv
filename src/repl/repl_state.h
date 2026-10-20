#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "encoding/replog_codec.h"
#include "storage/storage.h"

// Replication identity (M6a D7) and the PSYNC decision rule (D8).
namespace tideskv::repl {

inline constexpr size_t kReplIdLength = 40;
inline constexpr size_t kMaxHistory = 4;
inline constexpr std::string_view kSysReplId = "repl_id";
inline constexpr std::string_view kSysReplHistory = "repl_history";
inline constexpr std::string_view kSysReplFirst = "repl_first";  // written by the trimmer (Task 14)
// F5: "1" when the data predates the replication log (a pre-M6a directory). Node-local; a full sync
// (M6b) clears it.
inline constexpr std::string_view kSysReplBaselineUnknown = "repl_baseline_unknown";

struct HistoryEntry {
  std::string replid;
  uint64_t end_seq = 0;  // last seq produced under replid
  bool operator==(const HistoryEntry&) const = default;
};
struct ReplIdentity {
  std::string replid;                 // 40 lowercase hex
  std::vector<HistoryEntry> history;  // newest first, <= kMaxHistory
};

std::string NewReplId();  // 40 random lowercase hex (std::random_device)
bool IsValidReplId(std::string_view id);
// Layout: u8 count, then count x (40-byte replid, 8-byte big-endian end_seq).
std::string EncodeHistory(const std::vector<HistoryEntry>& h);
Result<std::vector<HistoryEntry>> DecodeHistory(std::string_view raw);  // Corruption on defects

// Loads repl_id/repl_history from sys; when absent creates a fresh id and persists it (no replog: sys
// identity is node-local). Must run before ReplLog fences or accepts writes.
Result<ReplIdentity> LoadOrCreateIdentity(Storage& storage);
// The two sys PUTs that persist `id` (repl_id, repl_history), for a transaction that also does other work
// (a full-sync load, M6b D5). PersistIdentity commits exactly these.
std::vector<encoding::ReplOp> IdentityOps(const ReplIdentity& id);
// One raw transaction, both keys; deliberately not routed through ReplLog (identity is node-local and
// must not be replicated). On a replica this runs only on the applier thread or at startup, never
// concurrently with an apply.
Status PersistIdentity(Storage& storage, const ReplIdentity& id);
// The identity after `old` adopts `new_replid` at `last_seq` (D7): pushes (old.replid, last_seq) when
// the id changes and caps the history. Pure.
ReplIdentity Adopt(const ReplIdentity& old, std::string_view new_replid, uint64_t last_seq);
// Promotion: Adopt(old, NewReplId(), last_seq). Pure apart from the id.
ReplIdentity Promote(const ReplIdentity& old, uint64_t last_seq);

// R12: a node that starts as a master (no replicaof) takes a fresh replid before it accepts any
// connection: Promote(id, last_seq), persisted. A master that lost an unsynced tail (sync-mode interval,
// OS crash) then re-issues those seqs under the new id, so a replica that applied the lost ones is ahead
// of the history entry and gets FULLRESYNC instead of a silently diverging CONTINUE. On error nothing
// is stored and the node must not start.
Result<ReplIdentity> RotateForMasterStart(Storage& storage, const ReplIdentity& id, uint64_t last_seq);

// F5: whether this node's replication log describes its data. True when sys/repl_baseline_unknown is
// set, or when it is detected now: an empty log (last_seq == 0) over a non-empty keyspace (meta, subkey,
// zscore or expire), i.e. a directory written before M6a; the flag is then persisted (one raw txn, like
// the identity). Run once at startup, after ReplLog::Recover and before any commit.
Result<bool> LoadOrDetectBaselineUnknown(Storage& storage, uint64_t last_seq);

struct PsyncRequest {
  std::string replid;  // "?" = unknown
  uint64_t next_seq = 0;
  bool full_only = false;  // the replica's key layout is not this master's: never CONTINUE (it adopts ours)
};
struct LogBounds {
  uint64_t first_retained = 1;
  uint64_t last = 0;
  bool baseline_unknown = false;  // F5: the log does not describe the data it starts from
};
enum class PsyncDecision : uint8_t { kContinue, kFullResync };
PsyncDecision DecidePsync(const ReplIdentity& me, const LogBounds& log, const PsyncRequest& req);

}  // namespace tideskv::repl
