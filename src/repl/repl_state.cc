#include "repl/repl_state.h"

#include <algorithm>
#include <random>

#include "common/coding.h"
#include "exec/repl_log.h"

namespace tideskv::repl {
namespace {

constexpr char kHex[] = "0123456789abcdef";

Result<std::string> GetSys(ReadView& view, std::string_view key, bool* found) {
  auto raw = view.Get(ColumnFamily::kSys, key);
  *found = raw.ok();
  if (raw.ok() || raw.status().IsNotFound()) return raw.ok() ? std::move(*raw) : std::string();
  return raw.status();
}

// Any key in a keyspace CF (sys and the replog are node-local or the log itself).
Result<bool> HasKeyspaceData(ReadView& view) {
  for (ColumnFamily cf :
       {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore, ColumnFamily::kExpire}) {
    auto it = view.NewIterator(cf);
    if (!it.ok()) return it.status();
    const Status st = (*it)->SeekToFirst();
    if (!st.ok() && !st.IsNotFound()) return st;
    if (st.ok() && (*it)->Valid()) return true;
  }
  return false;
}

}  // namespace

std::string NewReplId() {
  std::random_device rd;
  std::string id;
  id.reserve(kReplIdLength);
  while (id.size() < kReplIdLength) {
    const uint32_t word = rd();
    for (int shift = 0; shift < 32 && id.size() < kReplIdLength; shift += 4) {
      id.push_back(kHex[(word >> shift) & 0xF]);
    }
  }
  return id;
}

bool IsValidReplId(std::string_view id) {
  return id.size() == kReplIdLength && std::all_of(id.begin(), id.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string EncodeHistory(const std::vector<HistoryEntry>& h) {
  std::string out;
  out.push_back(static_cast<char>(h.size()));
  for (const HistoryEntry& e : h) {
    out += e.replid;
    coding::PutFixed64BE(&out, e.end_seq);
  }
  return out;
}

Result<std::vector<HistoryEntry>> DecodeHistory(std::string_view raw) {
  constexpr size_t kEntrySize = kReplIdLength + 8;
  if (raw.empty()) return Status::Corruption("replication history is empty");
  const size_t count = static_cast<uint8_t>(raw.front());
  raw.remove_prefix(1);
  if (count > kMaxHistory) return Status::Corruption("replication history has too many entries");
  if (raw.size() != count * kEntrySize) return Status::Corruption("replication history has the wrong length");
  std::vector<HistoryEntry> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    HistoryEntry e;
    e.replid = std::string(raw.substr(0, kReplIdLength));
    if (!IsValidReplId(e.replid)) return Status::Corruption("replication history holds an invalid replid");
    raw.remove_prefix(kReplIdLength);
    coding::GetFixed64BE(&raw, &e.end_seq);
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<encoding::ReplOp> IdentityOps(const ReplIdentity& id) {
  return {{encoding::ReplOpType::kPut, ColumnFamily::kSys, std::string(kSysReplId), id.replid},
          {encoding::ReplOpType::kPut, ColumnFamily::kSys, std::string(kSysReplHistory),
           EncodeHistory(id.history)}};
}

Status PersistIdentity(Storage& storage, const ReplIdentity& id) {
  auto txn = storage.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = ApplyReplOps(**txn, IdentityOps(id)); !st.ok()) return st;
  return (*txn)->Commit();
}

Result<ReplIdentity> LoadOrCreateIdentity(Storage& storage) {
  ReplIdentity id;
  {
    auto view = storage.BeginRead();
    if (!view.ok()) return view.status();
    bool have_id = false;
    bool have_history = false;
    auto raw_id = GetSys(**view, kSysReplId, &have_id);
    if (!raw_id.ok()) return raw_id.status();
    auto raw_history = GetSys(**view, kSysReplHistory, &have_history);
    if (!raw_history.ok()) return raw_history.status();
    if (have_id) {
      if (!IsValidReplId(*raw_id)) return Status::Corruption("sys/repl_id is not a valid replid");
      id.replid = std::move(*raw_id);
      if (have_history) {
        auto history = DecodeHistory(*raw_history);
        if (!history.ok()) return history.status();
        id.history = std::move(*history);
      }
      return id;
    }
  }
  id.replid = NewReplId();
  if (Status st = PersistIdentity(storage, id); !st.ok()) return st;
  return id;
}

ReplIdentity Adopt(const ReplIdentity& old, std::string_view new_replid, uint64_t last_seq) {
  ReplIdentity next{std::string(new_replid), old.history};
  if (new_replid == old.replid) return next;
  next.history.insert(next.history.begin(), HistoryEntry{old.replid, last_seq});
  if (next.history.size() > kMaxHistory) next.history.resize(kMaxHistory);
  return next;
}

ReplIdentity Promote(const ReplIdentity& old, uint64_t last_seq) { return Adopt(old, NewReplId(), last_seq); }

Result<ReplIdentity> RotateForMasterStart(Storage& storage, const ReplIdentity& id, uint64_t last_seq) {
  ReplIdentity rotated = Promote(id, last_seq);
  if (Status st = PersistIdentity(storage, rotated); !st.ok()) return st;
  return rotated;
}

Result<bool> LoadOrDetectBaselineUnknown(Storage& storage, uint64_t last_seq) {
  {
    auto view = storage.BeginRead();
    if (!view.ok()) return view.status();
    bool flagged = false;
    auto raw = GetSys(**view, kSysReplBaselineUnknown, &flagged);
    if (!raw.ok()) return raw.status();
    if (flagged || last_seq != 0) return flagged;
    auto has_keys = HasKeyspaceData(**view);
    if (!has_keys.ok() || !*has_keys) return has_keys;
  }
  auto txn = storage.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = (*txn)->Put(ColumnFamily::kSys, kSysReplBaselineUnknown, "1"); !st.ok()) return st;
  if (Status st = (*txn)->Commit(); !st.ok()) return st;
  return true;
}

PsyncDecision DecidePsync(const ReplIdentity& me, const LogBounds& log, const PsyncRequest& req) {
  const bool in_backlog = req.next_seq >= log.first_retained && req.next_seq <= log.last + 1;
  if (!in_backlog) return PsyncDecision::kFullResync;
  if (req.replid == "?") {  // an empty replica: only a log that starts at seq 1 over no data can serve it
    return req.next_seq == 1 && log.first_retained == 1 && !log.baseline_unknown ? PsyncDecision::kContinue
                                                                                 : PsyncDecision::kFullResync;
  }
  if (req.replid == me.replid) return PsyncDecision::kContinue;
  for (const HistoryEntry& e : me.history) {
    if (e.replid == req.replid) {
      return req.next_seq - 1 <= e.end_seq ? PsyncDecision::kContinue : PsyncDecision::kFullResync;
    }
  }
  return PsyncDecision::kFullResync;
}

}  // namespace tideskv::repl
