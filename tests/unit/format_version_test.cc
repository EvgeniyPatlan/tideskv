#include "types/format_version.h"

#include <gtest/gtest.h>

#include "storage/memory_storage.h"

namespace tideskv {
namespace {

std::string SysValue(InMemoryStorage& s) {
  const auto tables = s.Current();
  const auto& sys = (*tables)[CfIndex(ColumnFamily::kSys)];
  const auto it = sys.find(std::string(kFormatVersionKey));
  return it == sys.end() ? "<missing>" : it->second;
}

TEST(FormatVersion, WrittenOnFreshStore) {
  InMemoryStorage s;
  ASSERT_TRUE(EnsureFormatVersion(s).ok());
  EXPECT_EQ(SysValue(s), "1");
}

TEST(FormatVersion, SameVersionIsAccepted) {
  InMemoryStorage s;
  ASSERT_TRUE(EnsureFormatVersion(s).ok());
  EXPECT_TRUE(EnsureFormatVersion(s).ok());
}

TEST(FormatVersion, UnknownVersionIsRefused) {
  InMemoryStorage s;
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, kFormatVersionKey, "99").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  const Status st = EnsureFormatVersion(s);
  EXPECT_EQ(st.code(), StatusCode::kCorruption);
  EXPECT_NE(st.message().find("format version 99"), std::string::npos) << st.message();
}

}  // namespace
}  // namespace tideskv
