#include "types/format_version.h"

#include <gtest/gtest.h>

#include "encoding/key_codec.h"
#include "encoding/replog_codec.h"
#include "storage/memory_storage.h"

namespace tideskv {
namespace {

using encoding::KeyLayout;

std::string SysValue(InMemoryStorage& s) {
  const auto tables = s.Current();
  const auto& sys = (*tables)[CfIndex(ColumnFamily::kSys)];
  const auto it = sys.find(std::string(kFormatVersionKey));
  return it == sys.end() ? "<missing>" : it->second;
}

void Put(InMemoryStorage& s, ColumnFamily cf, std::string_view key, std::string_view value) {
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(cf, key, value).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

TEST(FormatVersion, AFreshStoreTakesTheConfiguredLayout) {
  InMemoryStorage standalone;
  auto off = OpenFormat(standalone, /*cluster_enabled=*/false);
  ASSERT_TRUE(off.ok()) << off.status().ToString();
  EXPECT_EQ(off->layout, KeyLayout::kSlotless);
  EXPECT_EQ(off->outcome, FormatOutcome::kCreated);
  EXPECT_EQ(SysValue(standalone), "2");
  InMemoryStorage cluster;
  auto on = OpenFormat(cluster, /*cluster_enabled=*/true);
  ASSERT_TRUE(on.ok());
  EXPECT_EQ(on->layout, KeyLayout::kSlotted);
  EXPECT_EQ(SysValue(cluster), "1");
}

TEST(FormatVersion, TheSameVersionIsKept) {
  for (bool cluster : {false, true}) {
    InMemoryStorage s;
    ASSERT_TRUE(OpenFormat(s, cluster).ok());
    Put(s, ColumnFamily::kMeta, std::string("\x00k", 2), "v");
    auto again = OpenFormat(s, cluster);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->outcome, FormatOutcome::kKept);
    EXPECT_EQ(again->layout, cluster ? KeyLayout::kSlotted : KeyLayout::kSlotless);
  }
}

TEST(FormatVersion, ALegacySlottedDirWithKeysRunsSlottedStandalone) {
  InMemoryStorage s;
  Put(s, ColumnFamily::kSys, kFormatVersionKey, "1");
  Put(s, ColumnFamily::kMeta, std::string("\x00\x0c\xe4k", 4), "v");
  auto r = OpenFormat(s, /*cluster_enabled=*/false);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->layout, KeyLayout::kSlotted);
  EXPECT_EQ(r->outcome, FormatOutcome::kLegacy);
  EXPECT_EQ(SysValue(s), "1");
}

TEST(FormatVersion, ASlotlessDirWithKeysRefusesClusterMode) {
  InMemoryStorage s;
  Put(s, ColumnFamily::kSys, kFormatVersionKey, "2");
  Put(s, ColumnFamily::kExpire, std::string("\x00\x00\x00\x00\x00\x00\x00\x00\x01k", 10),
      "x");  // any keyspace CF counts
  auto r = OpenFormat(s, /*cluster_enabled=*/true);
  EXPECT_EQ(r.status().code(), StatusCode::kInvalidArgument);
  EXPECT_NE(r.status().message().find("standalone key layout (format 2)"), std::string::npos)
      << r.status().message();
  EXPECT_EQ(SysValue(s), "2");
}

TEST(FormatVersion, AnEmptyDirSwitchesToTheConfiguredLayout) {
  for (const auto& [stored, cluster, want, layout] :
       {std::tuple{"1", false, "2", KeyLayout::kSlotless}, std::tuple{"2", true, "1", KeyLayout::kSlotted}}) {
    InMemoryStorage s;
    Put(s, ColumnFamily::kSys, kFormatVersionKey, stored);
    Put(s, ColumnFamily::kSys, "oid_hwm", "x");  // sys entries do not count
    // A baseline record (no op) does not count either: no PSYNC can ship a key from it.
    Put(s, ColumnFamily::kReplog, encoding::ReplogKey(7), encoding::EncodeReplogBatch({}, 1, 0));
    auto r = OpenFormat(s, cluster);
    ASSERT_TRUE(r.ok()) << r.status().ToString();
    EXPECT_EQ(r->outcome, FormatOutcome::kSwitched);
    EXPECT_EQ(r->layout, layout);
    EXPECT_EQ(SysValue(s), want);
  }
}

TEST(FormatVersion, ALoggedOpKeepsTheLayoutOfAnEmptyKeyspace) {  // a CONTINUE could still ship it
  InMemoryStorage s;
  Put(s, ColumnFamily::kSys, kFormatVersionKey, "1");
  const std::vector<encoding::ReplOp> ops = {
      {encoding::ReplOpType::kDelete, ColumnFamily::kMeta, std::string("\x00\x0c\xe4k", 4), ""}};
  Put(s, ColumnFamily::kReplog, encoding::ReplogKey(3), encoding::EncodeReplogBatch({}, 1, 0));
  Put(s, ColumnFamily::kReplog, encoding::ReplogKey(4), encoding::EncodeReplogBatch(ops, 1, 0));
  auto r = OpenFormat(s, /*cluster_enabled=*/false);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->outcome, FormatOutcome::kLegacy);
  EXPECT_EQ(SysValue(s), "1");
}

TEST(FormatVersion, AnyKeyspaceCfMakesTheDirNonEmpty) {
  for (ColumnFamily cf : {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore}) {
    InMemoryStorage s;
    Put(s, ColumnFamily::kSys, kFormatVersionKey, "1");
    Put(s, cf, "\x03whatever", "v");
    auto r = OpenFormat(s, /*cluster_enabled=*/false);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->outcome, FormatOutcome::kLegacy) << ColumnFamilyName(cf);
  }
}

TEST(FormatVersion, UnknownVersionIsRefused) {
  InMemoryStorage s;
  Put(s, ColumnFamily::kSys, kFormatVersionKey, "99");
  const Status st = OpenFormat(s, false).status();
  EXPECT_EQ(st.code(), StatusCode::kCorruption);
  EXPECT_NE(st.message().find("format version 99"), std::string::npos) << st.message();
}

TEST(FormatVersion, VersionNamesFollowTheLayout) {
  EXPECT_EQ(FormatVersionFor(KeyLayout::kSlotted), "1");
  EXPECT_EQ(FormatVersionFor(KeyLayout::kSlotless), "2");
}

}  // namespace
}  // namespace tideskv
