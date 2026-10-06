#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tideskv {

// Column families (spec §4.1). Values are stable: they appear in replog records.
enum class ColumnFamily : uint8_t { kMeta = 0, kSubkey, kZScore, kExpire, kReplog, kSys };

inline constexpr size_t kColumnFamilyCount = 6;
inline constexpr std::array<ColumnFamily, kColumnFamilyCount> kAllColumnFamilies = {
    ColumnFamily::kMeta,   ColumnFamily::kSubkey, ColumnFamily::kZScore,
    ColumnFamily::kExpire, ColumnFamily::kReplog, ColumnFamily::kSys};

inline constexpr size_t CfIndex(ColumnFamily cf) { return static_cast<size_t>(cf); }

std::string_view ColumnFamilyName(ColumnFamily cf);

}  // namespace tideskv
