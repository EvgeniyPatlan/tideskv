#pragma once
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/commit_listener.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "exec/repl_log.h"
#include "server/watch.h"
#include "storage/storage.h"
#include "support/fake_clock.h"
#include "types/digest.h"

namespace tideskv::test {

// Remembers every range delete it is told about.
class RecordingListener final : public CommitListener {
 public:
  void OnCommitted(const std::vector<RangeDelete>& deletes) override {
    seen.insert(seen.end(), deletes.begin(), deletes.end());
  }
  std::vector<RangeDelete> seen;
};

// A replica's stack over `storage` (M6a D11): Engine + ReplLog with local writes fenced, with a
// WatchRegistry and a recording CommitListener to observe what an apply reports.
struct ReplicaStack {
  explicit ReplicaStack(Storage& s) : storage(s) {
    EXPECT_TRUE(log.Recover().ok());
    log.FenceLocalWrites(true);
  }

  Storage& storage;
  LockManager locks;
  FakeClock clock;
  WatchRegistry watches;
  RecordingListener listener;
  ReplLog log{storage, clock};
  Engine engine{storage, locks, clock, &listener, &watches, &log};
};

// replog[seq] of `s`, verbatim.
inline Result<std::string> ReplogEntry(Storage& s, uint64_t seq) {
  auto view = s.BeginRead();
  if (!view.ok()) return view.status();
  return (*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(seq));
}

// Applies master replog entries replica.last()+1 .. master_last on the replica, in order.
inline Status ShipAll(Storage& master, uint64_t master_last, Engine& replica) {
  for (uint64_t seq = replica.repl_log()->last() + 1; seq <= master_last; ++seq) {
    auto payload = ReplogEntry(master, seq);
    if (!payload.ok()) return payload.status();
    if (Status st = replica.ApplyReplicated(seq, *payload); !st.ok()) return st;
  }
  return Status::Ok();
}

// DEBUG DIGEST of what `engine` serves, read from one snapshot.
inline std::string DigestOf(Engine& engine) {
  std::string hex;
  const Status st = engine.Snapshot([&](ReadView& view, int64_t) {
    auto d = DatasetDigest(view);
    if (!d.ok()) return d.status();
    hex = DigestHex(*d);
    return Status::Ok();
  });
  EXPECT_TRUE(st.ok()) << st.ToString();
  return hex;
}

}  // namespace tideskv::test
