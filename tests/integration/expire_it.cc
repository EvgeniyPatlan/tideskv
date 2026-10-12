#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

TEST(ExpireIT, ExpireTtlPersistInRealTime) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
  // A wide deadline for the PTTL range check, so a slow (sanitizer) run can't see it lapse...
  EXPECT_EQ(c->Command({"PEXPIRE", "k", "2000"}).integer, 1);
  const int64_t pttl = c->Command({"PTTL", "k"}).integer;
  EXPECT_GT(pttl, 0);
  EXPECT_LE(pttl, 2000);
  // ...then a short one to watch it really expire.
  EXPECT_EQ(c->Command({"PEXPIRE", "k", "100"}).integer, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(c->Command({"TTL", "k"}).integer, -2);
  EXPECT_EQ(c->Command({"SET", "p", "v", "EX", "100"}).str, "OK");
  EXPECT_EQ(c->Command({"PERSIST", "p"}).integer, 1);
  EXPECT_EQ(c->Command({"TTL", "p"}).integer, -1);
}

}  // namespace
}  // namespace tideskv::test
