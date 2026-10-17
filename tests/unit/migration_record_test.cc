#include "cluster/migration_record.h"

#include <gtest/gtest.h>

#include <string>

#include "storage/memory_storage.h"

namespace tideskv::cluster {
namespace {

const std::string kB(40, 'b');

MigrationRecord Full() {
  MigrationRecord r;
  r.job = SourceJob{{100, 200}, kB, "10.0.0.2", 7001, MigrateState::kSuccess};
  r.migrated = {{{100, 200}, kB, "10.0.0.2", 7001}, {{5, 5}, kB, "host-with-a-long-name.example", 65535}};
  r.cleanup = {{16383, 16383}};
  r.import = ImportJob{{300, 300}, ImportState::kError};
  r.imported = {{0, 4}, {400, 401}};
  return r;
}

TEST(MigrationRecord, RoundTripsEveryField) {  // D8
  const MigrationRecord r = Full();
  auto back = DecodeMigrationRecord(EncodeMigrationRecord(r));
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ(*back, r);
  auto empty = DecodeMigrationRecord(EncodeMigrationRecord(MigrationRecord{}));
  ASSERT_TRUE(empty.ok());
  EXPECT_EQ(*empty, MigrationRecord{});
  const encoding::ReplOp put = MigrationRecordPut(r);
  EXPECT_EQ(put.type, encoding::ReplOpType::kPut);
  EXPECT_EQ(put.cf, ColumnFamily::kSys);
  EXPECT_EQ(put.key, kSysMigration);
  EXPECT_EQ(put.value, EncodeMigrationRecord(r));
}

TEST(MigrationRecord, ARunningJobIsNotPersistedAndARecordFencesItsRanges) {  // D8
  SlotStates s;
  s.job = SourceJob{{1, 2}, kB, "h", 1, MigrateState::kStart};
  s.migrated = {{{9, 9}, kB, "h", 1}};
  s.cleanup = {{20, 21}};
  EXPECT_FALSE(RecordOf(s).job.has_value());
  s.job->state = MigrateState::kFail;
  EXPECT_TRUE(RecordOf(s).job.has_value());
  const SlotStates back = StatesFromRecord(RecordOf(s));
  EXPECT_TRUE(back.Fenced(9));
  EXPECT_TRUE(back.Fenced(20));
  EXPECT_TRUE(back.Fenced(21));
  EXPECT_FALSE(back.Fenced(1));
  EXPECT_EQ(back.migrated, s.migrated);
}

TEST(MigrationRecord, ADamagedRecordIsCorruption) {
  const std::string good = EncodeMigrationRecord(Full());
  EXPECT_EQ(DecodeMigrationRecord("").status().code(), StatusCode::kCorruption);
  EXPECT_EQ(DecodeMigrationRecord(std::string("\x02") + good.substr(1)).status().code(),
            StatusCode::kCorruption);
  EXPECT_EQ(DecodeMigrationRecord(good.substr(0, good.size() - 1)).status().code(), StatusCode::kCorruption);
  EXPECT_EQ(DecodeMigrationRecord(good + "x").status().code(), StatusCode::kCorruption);
  MigrationRecord running;
  running.job = SourceJob{{1, 1}, kB, "h", 1, MigrateState::kStart};  // RecordOf never writes one
  EXPECT_EQ(DecodeMigrationRecord(EncodeMigrationRecord(running)).status().code(), StatusCode::kCorruption);
  MigrationRecord inverted;
  inverted.cleanup = {{9, 3}};
  EXPECT_EQ(DecodeMigrationRecord(EncodeMigrationRecord(inverted)).status().code(), StatusCode::kCorruption);
}

TEST(MigrationRecord, LoadFindsNothingOnAFreshNode) {
  InMemoryStorage s;
  auto loaded = LoadMigrationRecord(s);
  ASSERT_TRUE(loaded.ok());
  EXPECT_FALSE(loaded->has_value());
}

}  // namespace
}  // namespace tideskv::cluster
