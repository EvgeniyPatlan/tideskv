#include "cluster/nodes_parser.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

#include "cluster/cluster_errors.h"
#include "common/string_util.h"

namespace tideskv::cluster {
namespace {

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

// Kvrocks util::Split: empty tokens are dropped.
std::vector<std::string_view> SplitDropEmpty(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i <= s.size()) {
    const size_t j = std::min(s.find(sep, i), s.size());
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

bool ParseSlot(std::string_view s, uint16_t* out) {
  int64_t v = 0;
  if (!ParseInt64(s, &v) || v < 0 || v >= kSlotCount) return false;
  *out = static_cast<uint16_t>(v);
  return true;
}

bool IsHexNodeId(std::string_view s) {  // Kvrocks's \b[a-fA-F0-9]{40}\b, matched against the whole token
  return s.size() == kNodeIdLength && std::all_of(s.begin(), s.end(), [](char c) {
           return std::isxdigit(static_cast<unsigned char>(c)) != 0;
         });
}

Status ParseFlags(std::string_view flags, ClusterNode* n) {
  bool role = false;
  for (std::string_view f : SplitDropEmpty(flags, ',')) {
    if (EqualsIgnoreCase(f, "master")) {
      n->role = NodeRole::kMaster;
      role = true;
    } else if (EqualsIgnoreCase(f, "slave") || EqualsIgnoreCase(f, "replica")) {
      n->role = NodeRole::kSlave;
      role = true;
    } else if (EqualsIgnoreCase(f, "fail")) {
      n->failed = true;
    }  // myself, pfail, handshake, noaddr, nofailover, noflags: ignored
  }
  return role ? Status::Ok() : Err(kErrInvalidNodeRole);
}

Status AssignToken(std::string_view token, int16_t index, std::array<int16_t, kSlotCount>& owner) {
  const auto parts = SplitDropEmpty(token, '-');
  uint16_t a = 0;
  uint16_t b = 0;
  if (parts.size() == 1) {
    if (IsHexNodeId(token)) return Err(kErrMissingNewline);
    if (!ParseSlot(parts[0], &a)) return Err(kErrSlotOutOfRange);
    b = a;
  } else if (parts.size() == 2) {
    if (!ParseSlot(parts[0], &a) || !ParseSlot(parts[1], &b) || a >= b) return Err(kErrSlotOutOfRange);
  } else {
    return Err(kErrSlotOutOfRange);
  }
  for (uint32_t s = a; s <= b; ++s) {
    if (owner[s] != -1) return Err(kErrSlotOverlapped);
    owner[s] = index;
  }
  return Status::Ok();
}

Status ParseHead(const std::vector<std::string_view>& f, ClusterNode* n) {
  if (f[0].size() != kNodeIdLength) return Err(kErrInvalidNodeId);
  n->id = std::string(f[0]);
  n->host = std::string(f[1]);
  uint64_t port = 0;
  if (!ParseUint64(f[2], &port) || port > std::numeric_limits<uint16_t>::max())
    return Err(kErrInvalidNodePort);
  n->port = static_cast<uint16_t>(port);
  if (Status st = ParseFlags(f[3], n); !st.ok()) return st;
  const bool master = n->role == NodeRole::kMaster;
  if ((master && f[4] != "-") || (!master && f[4].size() != kNodeIdLength)) return Err(kErrInvalidNodeId);
  if (!master) n->master_id = std::string(f[4]);
  return Status::Ok();
}

Status ParseLine(std::string_view line, ParsedNodes& out) {
  const auto f = SplitDropEmpty(line, ' ');
  if (f.size() < 5) return Err(kErrInvalidNodesInfo);
  ClusterNode n;
  if (Status st = ParseHead(f, &n); !st.ok()) return st;
  if (n.role == NodeRole::kSlave && f.size() != 5) return Err(kErrInvalidNodesInfo);
  const bool duplicate = std::any_of(out.nodes.begin(), out.nodes.end(),
                                     [&](const ClusterNode& seen) { return seen.id == n.id; });
  if (duplicate || out.nodes.size() >= static_cast<size_t>(std::numeric_limits<int16_t>::max())) {
    return Err(kErrInvalidNodesInfo);  // D4: TidesKV refuses a repeated id (Kvrocks keeps the first)
  }
  const auto index = static_cast<int16_t>(out.nodes.size());
  for (size_t i = 5; i < f.size(); ++i) {
    if (Status st = AssignToken(f[i], index, out.owner); !st.ok()) return st;
  }
  out.nodes.push_back(std::move(n));
  return Status::Ok();
}

// Kvrocks ParseInt's reasons, for "Invalid slot id: <reason>".
bool SlotId(std::string_view s, uint16_t* out, std::string* why) {
  int64_t v = 0;
  if (!ParseInt64(s, &v)) {
    *why = std::string(kSlotIdNonInteger);
    return false;
  }
  if (v < 0 || v >= kSlotCount) {
    *why = std::string(kSlotIdOutOfRange);
    return false;
  }
  *out = static_cast<uint16_t>(v);
  return true;
}

Result<SlotRange> ParseOneRange(std::string_view p) {
  std::string why;
  if (p.find('-') == std::string_view::npos) {
    uint16_t s = 0;
    if (!SlotId(p, &s, &why)) return Status::UserError(std::string(kErrInvalidSlotIdPrefix) + why);
    return SlotRange{s, s};
  }
  const std::string quoted = std::string(kErrInvalidSlotRangePrefix) + std::string(p) + "`. ";
  if (p.front() == '-' || p.back() == '-') {
    return Status::UserError(quoted + std::string(kInvalidSlotRangeDashSuffix));
  }
  const auto f = SplitDropEmpty(p, '-');
  if (f.size() != 2) return Status::UserError(quoted + std::string(kInvalidSlotRangeFormSuffix));
  uint16_t a = 0;
  uint16_t b = 0;
  if (!SlotId(f[0], &a, &why) || !SlotId(f[1], &b, &why) || a > b) {
    return Status::UserError(quoted + std::string(kInvalidSlotRangeOrderSuffix));
  }
  return SlotRange{a, b};
}

}  // namespace

Result<ParsedNodes> ParseNodes(std::string_view text) {
  const auto lines = SplitDropEmpty(text, '\n');
  if (lines.empty()) return Err(kErrInvalidNodesInfo);
  ParsedNodes out;
  out.owner.fill(-1);
  for (std::string_view line : lines) {
    if (Status st = ParseLine(line, out); !st.ok()) return st;
  }
  return out;
}

std::string SerializeNodes(const Topology& t) {
  std::string out;
  for (size_t i = 0; i < t.nodes.size(); ++i) {
    const ClusterNode& n = t.nodes[i];
    const bool master = n.role == NodeRole::kMaster;
    out += n.id + " " + n.host + " " + std::to_string(n.port) + " " + (master ? "master" : "slave");
    if (n.failed) out += ",fail";
    out += master ? std::string(" -") : " " + n.master_id;
    const std::string ranges = master ? FormatRanges(t.SlotsOf(static_cast<int>(i))) : std::string();
    if (!ranges.empty()) out += " " + ranges;
    out += "\n";
  }
  return out;
}

Result<std::vector<SlotRange>> ParseSlotRanges(std::string_view text) {
  if (text.empty()) return Err(kErrNoSlotsToParse);
  const auto parts = SplitDropEmpty(text, ' ');
  if (parts.empty()) {
    return Status::UserError(std::string(kErrInvalidSlotsPrefix) + std::string(text) +
                             std::string(kErrInvalidSlotsSuffix));
  }
  std::vector<SlotRange> out;
  for (std::string_view p : parts) {
    auto r = ParseOneRange(p);
    if (!r.ok()) return r.status();
    out.push_back(*r);
  }
  return out;
}

}  // namespace tideskv::cluster
