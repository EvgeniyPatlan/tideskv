#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// kvrocks-controller v1.3.0 GetClusterInfo (store/cluster_node.go:176-208) and ParseSlotRange
// (store/slot.go:115-155), transcribed (M7b Review Focus #5). ok == false is the parse error that makes the
// controller count a failed probe and, after max_ping_count of them, fail a master over (KM §1.4).
namespace tideskv::test {

struct ControllerClusterInfo {
  bool ok = true;
  int64_t epoch = -1;
  std::string migrating_slot;  // as read; "" when absent
  std::string migrating_state;
};

// Go strconv.Atoi on a decimal int: an optional sign, then digits only.
inline bool GoAtoi(std::string_view v, int64_t* out) {
  if (v.empty()) return false;
  const size_t from = (v[0] == '-' || v[0] == '+') ? 1 : 0;
  if (from == v.size()) return false;
  int64_t n = 0;
  for (size_t i = from; i < v.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(v[i])) == 0 || n > (int64_t{1} << 40)) return false;
    n = n * 10 + (v[i] - '0');
  }
  *out = v[0] == '-' ? 0 - n : n;
  return true;
}

inline bool ControllerParsesSlotRange(std::string_view s) {
  if (std::count(s.begin(), s.end(), '-') > 1) return false;
  const size_t dash = s.find('-');
  int64_t start = 0;
  int64_t stop = 0;
  if (dash == std::string_view::npos) return GoAtoi(s, &start) && start >= 0 && start <= 16383;
  if (!GoAtoi(s.substr(0, dash), &start) || !GoAtoi(s.substr(dash + 1), &stop)) return false;
  return start <= stop && start >= 0 && stop <= 16383;
}

inline std::vector<std::string> GoSplit(std::string_view s, std::string_view sep) {  // keeps empty fields
  std::vector<std::string> out;
  size_t pos = 0;
  for (;;) {
    const size_t at = s.find(sep, pos);
    out.emplace_back(s.substr(pos, at == std::string_view::npos ? std::string_view::npos : at - pos));
    if (at == std::string_view::npos) return out;
    pos = at + sep.size();
  }
}

inline std::string GoTrimSpace(std::string_view v) {
  const size_t b = v.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  return std::string(v.substr(b, v.find_last_not_of(" \t\r\n") - b + 1));
}

inline ControllerClusterInfo ParseLikeController(std::string_view info) {
  ControllerClusterInfo out;
  for (const std::string& line : GoSplit(info, "\r\n")) {
    const std::vector<std::string> fields = GoSplit(line, ":");
    if (fields.size() != 2) continue;
    const std::string value = GoTrimSpace(fields[1]);
    std::string key = GoTrimSpace(fields[0]);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    if (key == "cluster_current_epoch") {
      if (!GoAtoi(value, &out.epoch)) out.ok = false;
    } else if (key == "migrating_slot" || key == "migrating_slot(s)") {
      if (!ControllerParsesSlotRange(value)) out.ok = false;
      out.migrating_slot = value;
    } else if (key == "migrating_state") {
      out.migrating_state = value;
    }
    if (!out.ok) return out;
  }
  return out;
}

}  // namespace tideskv::test
