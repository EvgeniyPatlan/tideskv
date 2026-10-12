#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tideskv {

// Maps numeric SCAN cursors to scan positions (M2 plan D9): clients parse cursors as
// integers, so the encoded meta key can't be the cursor itself. Fixed size, oldest
// evicted; thread-safe; shared by all connections, like Redis cursors.
class CursorStore {
 public:
  static constexpr size_t kCapacity = 16384;

  uint64_t Save(std::string position);
  std::optional<std::string> Load(uint64_t cursor) const;

 private:
  struct Entry {
    uint64_t cursor = 0;
    std::string position;
  };
  mutable std::mutex mu_;
  std::vector<Entry> entries_ = std::vector<Entry>(kCapacity);
  uint64_t last_ = 0;
};

}  // namespace tideskv
