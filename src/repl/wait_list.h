#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "server/blocking.h"

namespace tideskv::repl {

// How many of `acks` are >= seq.
int64_t CountAtLeast(const std::vector<uint64_t>& acks, uint64_t seq);

// WAIT clients waiting for ACKs (M6b D9). Thread-safe; never calls a BlockedClient under its mutex (the
// lock order puts BlockedClient::mu_ first). A released waiter is removed, so ACKs that repeat a value
// (on_ack fires on every valid ACK, CF-M6b-7) release it at most once.
class WaitList {
 public:
  struct Ready {
    std::shared_ptr<BlockedClient> client;
    int64_t count = 0;  // the ACKs >= its seq: may exceed numreplicas, as in Redis
  };
  void Add(std::shared_ptr<BlockedClient> client, uint64_t seq, int64_t numreplicas);
  // Removes and returns the waiters that `acks` (the online replicas' ACKs) satisfy. The caller answers
  // them; one whose wait already ended answers nothing (Unblock returns false).
  std::vector<Ready> Release(const std::vector<uint64_t>& acks);
  // Drops waiters whose wait already ended (timeout, CLIENT UNBLOCK, disconnect, demotion).
  void Prune();
  size_t size() const;

 private:
  struct Entry {
    std::shared_ptr<BlockedClient> client;
    uint64_t seq = 0;
    int64_t numreplicas = 0;
  };
  mutable std::mutex mu_;
  std::vector<Entry> entries_;  // guarded by mu_
};

}  // namespace tideskv::repl
