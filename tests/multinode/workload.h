#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A seeded, multi-client write load against one server (M6a Task 15). Every client loops over a weighted
// mix of writes on a bounded keyspace, so keys collide often: strings with EX, counters, hashes, lists,
// sets, sorted sets, DEL, EXPIRE/PERSIST, RENAME, MULTI/EXEC, EVAL and, rarely, FLUSHDB of db 2.
// A client whose server goes away (kill -9) ends its loop; Start() after Stop() runs a fresh set of
// clients, and the `ack:<n>` numbering continues across runs. Error replies and clients that ended before
// Stop() are counted (Stats), so a test whose server stays up can tell a healthy load from a failed one.
namespace tideskv::test {

struct RespValue;

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
  struct Stats {
    uint64_t steps = 0;        // ops sent: one command, or MULTI .. EXEC, or SELECT .. SELECT
    uint64_t early_exits = 0;  // clients that ended before Stop(): no connection, EOF or a reply timeout
    // Every error reply, top level or inside EXEC's array, by its full text.
    std::map<std::string, uint64_t> errors;
    uint64_t error_count() const;
  };

  // Write commands the server answered without an error.
  uint64_t acked_writes() const { return acked_writes_.load(std::memory_order_relaxed); }
  Stats stats() const;  // over every run so far
  // The n of every `SET ack:<n>` the server answered +OK; none is ever deleted or expired.
  std::vector<uint64_t> acked_ack_keys() const;

 private:
  void RunClient(uint32_t seed);
  // One step's replies (one per command sent): counts its errors, and acknowledges it unless the last
  // reply is an error.
  void Record(const std::vector<RespValue>& replies, std::optional<uint64_t> ack);

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
  Stats stats_;                       // guarded by acked_mu_
  std::vector<std::thread> threads_;
};

}  // namespace tideskv::test
