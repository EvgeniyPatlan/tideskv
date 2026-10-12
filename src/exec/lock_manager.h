#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace tideskv {

// Striped key locks (spec §5.3, invariant I1). A command locks every key it writes;
// stripes are locked in ascending order with duplicates removed, so two commands can
// never wait on each other in a cycle.
class LockManager {
 public:
  static constexpr size_t kStripes = 4096;

  class Guard {
   public:
    Guard() = default;
    Guard(Guard&& other) noexcept : owner_(other.owner_), stripes_(std::move(other.stripes_)) {
      other.owner_ = nullptr;
    }
    Guard& operator=(Guard&&) = delete;
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard();

   private:
    friend class LockManager;
    Guard(LockManager* owner, std::vector<uint16_t> stripes) : owner_(owner), stripes_(std::move(stripes)) {}
    LockManager* owner_ = nullptr;
    std::vector<uint16_t> stripes_;  // ascending
  };

  static size_t StripeOf(uint8_t db, std::string_view key);
  [[nodiscard]] Guard Lock(uint8_t db, const std::vector<std::string_view>& keys);

 private:
  std::array<std::mutex, kStripes> mutexes_;
};

}  // namespace tideskv
