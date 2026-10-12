#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/version.h"
#include "support/temp_dir.h"

extern "C" {
#include <db.h>
}

TEST(Smoke, VersionIsSet) { EXPECT_EQ(tideskv::Version(), "0.1.0"); }

// Proves TidesDB links and opens. (tests/ may touch TidesDB; src/ may not.)
TEST(Smoke, TidesDbOpensAndCloses) {
  tideskv::test::TempDir tmp("tideskv-smoke");
  std::string path = (tmp.path() / "db").string();
  tidesdb_config_t cfg = tidesdb_default_config();
  cfg.db_path = path.data();
  cfg.log_level = TDB_LOG_WARN;
  tidesdb_t* db = nullptr;
  ASSERT_EQ(tidesdb_open(&cfg, &db), TDB_SUCCESS);
  EXPECT_EQ(tidesdb_close(db), TDB_SUCCESS);
}
