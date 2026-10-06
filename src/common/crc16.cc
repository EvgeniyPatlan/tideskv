#include "common/crc16.h"

#include <array>

namespace tideskv {
namespace {

constexpr std::array<uint16_t, 256> MakeTable() {
  std::array<uint16_t, 256> table{};
  for (int i = 0; i < 256; ++i) {
    auto crc = static_cast<uint16_t>(i << 8);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    table[static_cast<size_t>(i)] = crc;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

uint16_t Crc16(std::string_view data) {
  uint16_t crc = 0;
  for (unsigned char byte : data) {
    crc = static_cast<uint16_t>((crc << 8) ^ kTable[((crc >> 8) ^ byte) & 0xFF]);
  }
  return crc;
}

uint16_t KeySlot(std::string_view key) {
  const size_t open = key.find('{');
  if (open != std::string_view::npos) {
    const size_t close = key.find('}', open + 1);
    if (close != std::string_view::npos && close != open + 1) {
      key = key.substr(open + 1, close - open - 1);
    }
  }
  return static_cast<uint16_t>(Crc16(key) % kSlotCount);
}

}  // namespace tideskv
