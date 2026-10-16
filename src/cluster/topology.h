#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/crc16.h"

// The topology a controller pushes with CLUSTERX (Kvrocks model, M7a D3). Immutable once published:
// readers share it as std::shared_ptr<const Topology>. Per-node slot lists are derived from `owner`.
namespace tideskv::cluster {

inline constexpr size_t kNodeIdLength = 40;     // any bytes: controller ids are [A-Za-z0-9]{40}
inline constexpr int64_t kNoVersion = -1;       // before the first SETNODES (CLUSTERX VERSION "-1")
inline constexpr int kClusterPortIncr = 10000;  // the cport CLUSTER NODES shows (Kvrocks)

enum class NodeRole : uint8_t { kMaster, kSlave };

struct SlotRange {
  uint16_t start = 0;
  uint16_t end = 0;  // inclusive
  bool operator==(const SlotRange&) const = default;
};

struct ClusterNode {
  std::string id;
  std::string host;
  uint16_t port = 0;
  NodeRole role = NodeRole::kMaster;
  bool failed = false;    // the "fail" flag: display only (Kvrocks)
  std::string master_id;  // a slave's master (it may be absent from the topology); "" for a master
  bool operator==(const ClusterNode&) const = default;
};

struct Topology {
  int64_t version = kNoVersion;
  std::string myid;                       // SETNODEID or detected (D6); may be set while version == -1
  std::vector<ClusterNode> nodes;         // in the order of the nodes string
  std::array<int16_t, kSlotCount> owner;  // index into nodes (always a master); -1 = not served
  int myself = -1;                        // index of myid in nodes; -1 = absent (a router node)

  Topology() { owner.fill(-1); }
  bool initialized() const { return version >= 0; }
  const ClusterNode* Myself() const { return myself < 0 ? nullptr : &nodes[static_cast<size_t>(myself)]; }
  const ClusterNode* OwnerOf(uint16_t slot) const;
  int IndexOf(std::string_view id) const;  // -1 when absent
  const ClusterNode* Find(std::string_view id) const;
  std::vector<SlotRange> SlotsOf(int index) const;  // maximal runs, ascending
  size_t AssignedSlots() const;                     // cluster_slots_assigned
  size_t Size() const;                              // cluster_size: masters owning >= 1 slot
  void ResolveMyself();                             // myself = IndexOf(myid) once initialised, else -1
};

// "0-2 4 10000": runs as "a-b" (a < b) or "N", one space apart (CLUSTER NODES, the nodes string).
std::string FormatRanges(const std::vector<SlotRange>& ranges);

}  // namespace tideskv::cluster
