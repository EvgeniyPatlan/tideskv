#include "cluster/topology.h"

#include <algorithm>

namespace tideskv::cluster {

const ClusterNode* Topology::OwnerOf(uint16_t slot) const {
  const int16_t i = owner[slot];
  return i < 0 ? nullptr : &nodes[static_cast<size_t>(i)];
}

int Topology::IndexOf(std::string_view id) const {
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

const ClusterNode* Topology::Find(std::string_view id) const {
  const int i = IndexOf(id);
  return i < 0 ? nullptr : &nodes[static_cast<size_t>(i)];
}

std::vector<SlotRange> Topology::SlotsOf(int index) const {
  std::vector<SlotRange> out;
  for (uint32_t s = 0; s < kSlotCount; ++s) {
    if (owner[s] != index) continue;
    if (!out.empty() && out.back().end + 1u == s) {
      out.back().end = static_cast<uint16_t>(s);
    } else {
      out.push_back({static_cast<uint16_t>(s), static_cast<uint16_t>(s)});
    }
  }
  return out;
}

size_t Topology::AssignedSlots() const {
  return static_cast<size_t>(std::count_if(owner.begin(), owner.end(), [](int16_t o) { return o >= 0; }));
}

size_t Topology::Size() const {
  std::vector<bool> owns(nodes.size(), false);
  for (int16_t o : owner) {
    if (o >= 0) owns[static_cast<size_t>(o)] = true;
  }
  return static_cast<size_t>(std::count(owns.begin(), owns.end(), true));
}

void Topology::ResolveMyself() { myself = initialized() && !myid.empty() ? IndexOf(myid) : -1; }

std::string FormatRanges(const std::vector<SlotRange>& ranges) {
  std::string out;
  for (const SlotRange& r : ranges) {
    if (!out.empty()) out += ' ';
    out += std::to_string(r.start);
    if (r.end != r.start) out += "-" + std::to_string(r.end);
  }
  return out;
}

}  // namespace tideskv::cluster
