#include <gtest/gtest.h>

#include "support/it_helpers.h"
#include "support/test_server.h"

namespace tideskv::test {
namespace {

// A replica of a port nobody listens on: its link never comes up.
TEST(StaleReplicaIT, ServeStaleDataNoRefusesUntilSwitchedBack) {
  TestServer r(TIDESKV_SERVER_BIN);
  auto c = r.Connect();
  ASSERT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
  ASSERT_EQ(c->Command({"REPLICAOF", "127.0.0.1", "1"}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "k"}).str, "v");  // default yes: stale data is served
  ASSERT_EQ(c->Command({"CONFIG", "SET", "replica-serve-stale-data", "no"}).str, "OK");
  const RespValue get = c->Command({"GET", "k"});
  EXPECT_EQ(get.type, RespValue::Type::kError);
  EXPECT_EQ(get.str, "MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.");
  EXPECT_EQ(c->Command({"PING"}).type, RespValue::Type::kError);
  EXPECT_EQ(InfoString(*c, "replication", "role"), "slave");
  ASSERT_EQ(c->Command({"CONFIG", "SET", "slave-serve-stale-data", "yes"}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "k"}).str, "v");
  ASSERT_EQ(c->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
}

}  // namespace
}  // namespace tideskv::test
