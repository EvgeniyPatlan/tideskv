#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "multinode/cluster_client.h"

// A mixed-type write load on hash-tagged keys whose slots lie in one range (M7b D25), through ClusterClients
// that follow MOVED and retry TRYAGAIN. A key's family fixes its type, so no reply is WRONGTYPE; TTLs are
// long (600 s), so nothing expires while a test runs and a migrated range's digest can be compared across
// shards.
namespace tideskv::test::multinode {

// The first `n` tags "t<i>" (i = 0, 1, ...) whose slot lies in [first, last].
std::vector<std::string> TagsInRange(uint16_t first, uint16_t last, size_t n);

class ClusterMixedWorkload {
 public:
  ClusterMixedWorkload(const std::vector<uint16_t>& seeds, std::vector<std::string> tags, int clients,
                       uint32_t seed);
  ~ClusterMixedWorkload();  // Stop()
  ClusterMixedWorkload(const ClusterMixedWorkload&) = delete;
  ClusterMixedWorkload& operator=(const ClusterMixedWorkload&) = delete;
  void Start();
  void Stop();
  uint64_t steps() const { return steps_.load(); }
  uint64_t failures() const { return failures_.load(); }  // error replies after the client's retries
  std::map<std::string, uint64_t> errors() const;         // by text

 private:
  void Run(ClusterClient& client, uint32_t seed);
  std::vector<std::string> tags_;
  uint32_t seed_;
  uint32_t generation_ = 0;
  std::vector<std::unique_ptr<ClusterClient>> clients_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> steps_{0};
  std::atomic<uint64_t> failures_{0};
  mutable std::mutex mu_;
  std::map<std::string, uint64_t> errors_;  // guarded by mu_
};

}  // namespace tideskv::test::multinode
