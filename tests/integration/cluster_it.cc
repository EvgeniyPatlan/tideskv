#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/it_helpers.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// Cluster mode against a real server (M7a). Node ids are any 40 bytes (the controller uses [A-Za-z0-9]).
namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

TEST(ClusterIT, InfoAndHelloReportClusterMode) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--cluster-enabled", "yes"}});
  auto c = s.Connect();
  EXPECT_EQ(InfoField(*c, "cluster", "cluster_enabled"), 1);
  EXPECT_EQ(InfoString(*c, "server", "redis_mode"), "cluster");
  const RespValue got = c->Command({"CONFIG", "GET", "cluster-enabled"});
  ASSERT_EQ(got.elements.size(), 2u);
  EXPECT_EQ(got.elements[1].str, "yes");
  TestServer plain(TIDESKV_SERVER_BIN);
  auto p = plain.Connect();
  EXPECT_EQ(InfoField(*p, "cluster", "cluster_enabled"), 0);
  EXPECT_EQ(InfoString(*p, "server", "redis_mode"), "standalone");
}

}  // namespace
}  // namespace tideskv::test
