#pragma once
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/it_helpers.h"
#include "support/test_server.h"

namespace tideskv::test {

// A master and one replica whose link is up (C16: shared by the WAIT and min-replicas ITs).
struct ReplPairIT : ::testing::Test {
  ReplPairIT() {
    auto c = replica.Connect();
    EXPECT_EQ(c->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
    EXPECT_TRUE(WaitFor([&] { return InfoString(*c, "replication", "master_link_status") == "up"; },
                        std::chrono::seconds(10)));
  }
  TestServer master{TIDESKV_SERVER_BIN};
  TestServer replica{TIDESKV_SERVER_BIN};
};

}  // namespace tideskv::test
