#pragma once
#include <cstdint>

namespace tideskv {

enum class ReplRole : uint8_t { kMaster, kReplica };

// What command handlers ask of replication (M6a). Implemented by ReplicationManager (Task 12) and
// test::FakeReplication. Every method is thread-safe and non-blocking.
class Replication {
 public:
  virtual ~Replication() = default;
  virtual ReplRole role() const = 0;  // one atomic load
  bool IsReplica() const { return role() == ReplRole::kReplica; }
};

// The role before ReplicationManager exists (until Task 12): always a master.
class MasterOnlyReplication final : public Replication {
 public:
  ReplRole role() const override { return ReplRole::kMaster; }
};

}  // namespace tideskv
