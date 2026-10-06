#include <gtest/gtest.h>

#include <filesystem>

#include "common/version.h"

extern "C" {
#include <db.h>
}

TEST(Smoke, VersionIsSet) { EXPECT_EQ(tideskv::Version(), "0.1.0"); }

// Proves TidesDB links and opens. (tests/ may touch TidesDB; src/ may not.)
TEST(Smoke, TidesDbOpensAndCloses) {
  auto dir = std::filesystem::temp_directory_path() / "tideskv-smoke";
  std::filesystem::remove_all(dir);
  std::string path = dir.string();
  tidesdb_config_t cfg = tidesdb_default_config();
  cfg.db_path = path.data();
  cfg.log_level = TDB_LOG_WARN;
  tidesdb_t* db = nullptr;
  ASSERT_EQ(tidesdb_open(&cfg, &db), TDB_SUCCESS);
  EXPECT_EQ(tidesdb_close(db), TDB_SUCCESS);
  std::filesystem::remove_all(dir);
}
