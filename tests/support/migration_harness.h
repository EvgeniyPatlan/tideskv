#pragma once
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cluster/topology.h"
#include "migrate/migration_service.h"
#include "migrate/migration_transport.h"
#include "migrate/slot_migrator.h"
#include "repl/full_sync_frames.h"
#include "support/cluster_harness.h"
#include "types/slot_keys.h"

// Two in-process cluster nodes for migration tests (M7b): A, the source (port 7000), and B, the target
// (7001), both of kThreeNodes at version 1, the source's migrator talking to the target over an
// InProcessTransport.
namespace tideskv::test {

// The snapshot chunks of `range` on `source`, as its migrator sends them (D2): meta, subkey and zscore of the
// range, no expire CF, no sys record.
inline std::vector<std::string> RangeChunks(CommandHarness& source, cluster::SlotRange range,
                                            size_t max_bytes = 16 * 1024) {
  auto snap = source.services.repl_log_.SnapshotAtLast();
  EXPECT_TRUE(snap.ok());
  if (!snap.ok()) return {};
  repl::ChunkerOptions o;
  o.cfs = {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore};
  o.bounds = SlotRangeBounds(range.start, range.end);
  o.sys_keys = {};
  o.max_bytes = max_bytes;
  repl::SnapshotChunker chunker(*snap->view, 0, o);
  std::vector<std::string> out;
  for (;;) {
    auto next = chunker.Next(nullptr);
    EXPECT_TRUE(next.ok());
    if (!next.ok() || !next->has_value()) return out;
    out.push_back(**next);
  }
}

// A MigrationTransport into another in-process node: each Call runs as a command on `target` over one link
// session, so the target sees an import link as over TCP. Shutdown closes the link: its on_close runs.
class InProcessTransport final : public migrate::MigrationTransport {
 public:
  explicit InProcessTransport(CommandHarness& target) : link_(target) {}
  ~InProcessTransport() override { Shutdown(); }
  Status Call(const std::vector<std::string_view>& args) override {
    std::lock_guard lock(mu_);
    if (closed_) return Status::IOError("link closed");
    const std::string reply = link_.Run(ArgList(args.begin(), args.end()));
    if (reply.starts_with("+")) return Status::Ok();
    if (reply.starts_with("-")) return Status::UserError(reply.substr(1, reply.size() - 3));
    return Status::IOError("unexpected reply " + reply);
  }
  void Shutdown() override {
    std::function<void()> on_close;
    {
      std::lock_guard lock(mu_);
      if (closed_) return;
      closed_ = true;
      on_close = std::move(link_.session.on_close);
    }
    if (on_close) on_close();
  }

 private:
  std::mutex mu_;
  SecondClient link_;
  bool closed_ = false;
};

struct MigrationPair {
  MigrationPair() : MigrationPair(std::chrono::seconds(10)) {}
  // `import_worker_wait`, `failure_retry`: both nodes' MigrationDeps fields of those names (tests that hold
  // a worker shorten the first; tests that inject a storage failure shorten the second).
  explicit MigrationPair(std::chrono::milliseconds import_worker_wait,
                         std::chrono::milliseconds failure_retry = std::chrono::seconds(1))
      : source_cluster(source, 7000, import_worker_wait, failure_retry),
        target_cluster(target, 7001, import_worker_wait, failure_retry) {
    source_cluster.transport =
        [this](const std::string&, uint16_t, std::chrono::milliseconds,
               const std::atomic<bool>*) -> Result<std::unique_ptr<migrate::MigrationTransport>> {
      return std::unique_ptr<migrate::MigrationTransport>(std::make_unique<InProcessTransport>(target));
    };
    source_cluster.Push(kThreeNodes, 1);
    target_cluster.Push(kThreeNodes, 1);
  }
  migrate::MigrationService& source_migration() { return *source_cluster.migration; }
  migrate::MigrationService& target_migration() { return *target_cluster.migration; }

  CommandHarness source;
  CommandHarness target;
  ClusterHarness source_cluster;
  ClusterHarness target_cluster;
};

// Publishes the job's start state, as CLUSTERX MIGRATE does (D12), then starts the migrator.
inline Status StartJob(migrate::MigrationService& m, cluster::SlotRange r, migrate::MigrateDone done) {
  const migrate::MigrateJob job{r, kNodeB, "127.0.0.1", 7001};
  if (Status st = m.Mutate([&job](cluster::SlotStates& s) {
        s.job = cluster::SourceJob{job.range, job.dst_id, job.dst_host, job.dst_port,
                                   cluster::MigrateState::kStart};
      });
      !st.ok()) {
    return st;
  }
  return m.migrator().Start(job, std::move(done));
}

// Migrates `r` from the pair's source to its target and returns the outcome (10 s at most).
inline Status RunJob(MigrationPair& p, cluster::SlotRange r) {
  auto done = std::make_shared<std::promise<Status>>();
  if (Status st = StartJob(p.source_migration(), r, [done](const Status& s) { done->set_value(s); });
      !st.ok()) {
    return st;
  }
  auto outcome = done->get_future();
  if (outcome.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    return Status::TimedOut("the job did not end");
  }
  return outcome.get();
}

}  // namespace tideskv::test
