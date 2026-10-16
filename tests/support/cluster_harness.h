#pragma once
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cluster/cluster_manager.h"
#include "support/command_harness.h"

namespace tideskv::test {

inline const std::string kNodeA(40, 'a');
inline const std::string kNodeB(40, 'b');
inline const std::string kNodeC(40, 'c');
// A owns slots 0-8191 ("b" 3300, "bar" 5061, "c" 7365, "k2" 449, "hello" 866, "w" 3696); B owns 8192-16383
// ("foo" 12182, "a" 15495, "key" 12539, "x" 16287); C replicates A.
inline const std::string kThreeNodes = kNodeA + " 127.0.0.1 7000 master - 0-8191\n" + kNodeB +
                                       " 127.0.0.1 7001 master - 8192-16383\n" + kNodeC +
                                       " 127.0.0.1 7002 slave " + kNodeA + "\n";

// A ClusterManager over the harness's storage whose role hooks drive its FakeReplication; `port` picks
// which node of kThreeNodes this harness is (7000 = A, 7001 = B, 7002 = C).
struct ClusterHarness {
  explicit ClusterHarness(CommandHarness& h, uint16_t port = 7000) : h_(h) {
    cluster::ClusterManager::Options o;
    o.port = port;
    o.bind = "127.0.0.1";
    o.local_addresses = [] { return std::vector<std::string>{"127.0.0.1"}; };
    FakeReplication& r = h.services.replication_;
    o.is_replica = [&r] { return r.IsReplica(); };
    o.become_replica = [&r](const std::string& host, uint16_t p) { return r.BecomeReplica(host, p); };
    o.become_master = [&r] { return r.BecomeMaster(); };
    manager = std::make_unique<cluster::ClusterManager>(h.services.storage_, std::move(o));
    EXPECT_TRUE(manager->Load().ok());
    h.services.cluster_manager = manager.get();
  }
  ~ClusterHarness() { h_.services.cluster_manager = nullptr; }
  ClusterHarness(const ClusterHarness&) = delete;
  ClusterHarness& operator=(const ClusterHarness&) = delete;

  void Push(const std::string& nodes, int64_t version) {
    ASSERT_TRUE(manager->SetNodes(nodes, version, false).ok());
  }

  CommandHarness& h_;
  std::unique_ptr<cluster::ClusterManager> manager;
};

}  // namespace tideskv::test
