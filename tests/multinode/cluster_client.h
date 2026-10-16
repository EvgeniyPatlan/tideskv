#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/crc16.h"
#include "support/resp_client.h"
#include "support/test_server.h"

// Cluster helpers for the multi-node suites (M7a Task 14): topology pushes as kvrocks-controller sends them,
// and a minimal Redis Cluster client that follows MOVED, as go-redis' ClusterClient does.
namespace tideskv::test::multinode {

inline std::string NodeId(size_t index) { return std::string(39, 'n') + static_cast<char>('a' + index); }

struct NodeLine {
  size_t index = 0;   // into the servers vector
  int master = -1;    // -1: a master; else the index of its master
  std::string slots;  // masters only, e.g. "0-8191"; may be empty
};

std::string NodesString(const std::vector<const TestServer*>& servers, const std::vector<NodeLine>& lines);
// SETNODEID + SETNODES to each server (server i gets NodeId(i)), in `order` (default 0..n-1).
void PushTopology(const std::vector<const TestServer*>& servers, const std::string& nodes, int64_t version,
                  std::vector<size_t> order = {});

// Learns the slot map from CLUSTER SLOTS, sends each single-key command to the slot's owner, follows MOVED,
// and refreshes then retries after CLUSTERDOWN/READONLY/LOADING/TRYAGAIN/MASTERDOWN or a lost connection
// (bounded). Not thread-safe: one per thread.
class ClusterClient {
 public:
  explicit ClusterClient(std::vector<uint16_t> seeds);
  RespValue Command(const std::vector<std::string>& argv, const std::string& key);
  uint64_t redirects() const { return redirects_; }

 private:
  static constexpr int kMaxAttempts = 100;
  bool Refresh();
  RespClient* ConnectionTo(uint16_t port);
  std::vector<uint16_t> seeds_;
  std::map<uint16_t, std::unique_ptr<RespClient>> conns_;
  std::array<uint16_t, kSlotCount> owner_port_{};  // 0 = unknown
  uint64_t redirects_ = 0;
};

// N threads writing SET ack:<n> <n> through their own ClusterClient (kept across Start/Stop, so their slot
// maps go stale on a topology change and MOVED is exercised); every +OK is recorded.
class ClusterWorkload {
 public:
  ClusterWorkload(const std::vector<uint16_t>& seeds, int clients);
  ~ClusterWorkload();  // Stop()
  ClusterWorkload(const ClusterWorkload&) = delete;
  ClusterWorkload& operator=(const ClusterWorkload&) = delete;
  void Start();
  void Stop();
  std::vector<uint64_t> acked() const;
  uint64_t failures() const { return failures_.load(); }  // commands that ended in an error after retries
  uint64_t redirects() const;

 private:
  void Run(ClusterClient& client);
  std::vector<std::unique_ptr<ClusterClient>> clients_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> next_{0};
  std::atomic<uint64_t> failures_{0};
  mutable std::mutex mu_;
  std::vector<uint64_t> acked_;  // guarded by mu_
};

// Every acknowledged ack:<n> reads back as <n> through a fresh cluster client.
void ExpectAllAcked(const std::vector<uint16_t>& seeds, const std::vector<uint64_t>& acked);

}  // namespace tideskv::test::multinode
