#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// A seeded, multi-client write load against one server (M6a Task 15). Every client loops over a weighted
// mix of writes on a bounded keyspace, so keys collide often: strings with EX, counters, hashes, lists,
// sets, sorted sets, DEL, EXPIRE/PERSIST, RENAME, MULTI/EXEC, EVAL and, rarely, FLUSHDB of db 2.
// A client whose server goes away (kill -9) ends its loop quietly; Start() after Stop() runs a fresh set
// of clients, and the `ack:<n>` numbering continues across runs.
namespace tideskv::test {

class Workload {
 public:
  // kNoExpiry leaves out every command that sets a TTL, so the master's reaper never writes (failover
  // tests whose PSYNC outcome must not depend on an expiry racing the role change).
  enum class Mix : uint8_t { kAll, kNoExpiry };

  Workload(uint16_t port, uint32_t seed, int clients, Mix mix = Mix::kAll);
  ~Workload();  // Stop()
  Workload(const Workload&) = delete;
  Workload& operator=(const Workload&) = delete;

  void Start();
  void Stop();  // joins the clients
  // Write commands the server answered without an error.
  uint64_t acked_writes() const { return acked_writes_.load(std::memory_order_relaxed); }
  // The n of every `SET ack:<n>` the server answered +OK; none is ever deleted or expired.
  std::vector<uint64_t> acked_ack_keys() const;

 private:
  void RunClient(uint32_t seed);

  const uint16_t port_;
  const uint32_t seed_;
  const int clients_;
  const Mix mix_;
  uint32_t generation_ = 0;  // runs started so far: each run's clients get fresh seeds
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> acked_writes_{0};
  std::atomic<uint64_t> next_ack_{0};
  mutable std::mutex acked_mu_;
  std::vector<uint64_t> acked_keys_;  // guarded by acked_mu_
  std::vector<std::thread> threads_;
};

}  // namespace tideskv::test
