#pragma once
#include <cstdint>
#include <string_view>

namespace tideskv {

inline constexpr uint16_t kSlotCount = 16384;

// CRC16-CCITT (XMODEM): polynomial 0x1021, init 0. Same as Redis Cluster.
uint16_t Crc16(std::string_view data);

// Redis Cluster hash slot. If the key contains "{...}" with at least one byte
// between the first '{' and the next '}', only that substring is hashed.
uint16_t KeySlot(std::string_view key);

}  // namespace tideskv
