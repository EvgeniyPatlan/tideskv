#include "storage/tides_status.h"

#include <gtest/gtest.h>

namespace tideskv {
namespace {

TEST(FromTdb, MapsEngineCodes) {
  EXPECT_TRUE(FromTdb(0, "op").ok());
  EXPECT_EQ(FromTdb(-3, "op").code(), StatusCode::kNotFound);
  EXPECT_EQ(FromTdb(-2, "op").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(FromTdb(-12, "op").code(), StatusCode::kBusy);
  EXPECT_EQ(FromTdb(-7, "op").code(), StatusCode::kConflict);
  EXPECT_EQ(FromTdb(-16, "op").code(), StatusCode::kConflict);
  EXPECT_EQ(FromTdb(-15, "op").code(), StatusCode::kNoSpace);
  EXPECT_EQ(FromTdb(-4, "op").code(), StatusCode::kIOError);
  EXPECT_EQ(FromTdb(-5, "op").code(), StatusCode::kCorruption);
  EXPECT_EQ(FromTdb(-8, "op").code(), StatusCode::kInvalidArgument);  // TOO_LARGE
  EXPECT_EQ(FromTdb(-9, "op").code(), StatusCode::kNoSpace);          // MEMORY_LIMIT
  EXPECT_EQ(FromTdb(-13, "op").code(), StatusCode::kIOError);         // READONLY
  EXPECT_EQ(FromTdb(-14, "op").code(), StatusCode::kConflict);        // TXN_EXPIRED
  EXPECT_EQ(FromTdb(-17, "op").code(), StatusCode::kBusy);            // TOO_OLD
  EXPECT_EQ(FromTdb(-1, "op").code(), StatusCode::kInternal);
  EXPECT_EQ(FromTdb(-99, "op").code(), StatusCode::kInternal);
}

TEST(FromTdb, MessageNamesOperationAndCode) {
  EXPECT_EQ(FromTdb(-4, "commit").message(), "commit: tidesdb error -4");
}

}  // namespace
}  // namespace tideskv
