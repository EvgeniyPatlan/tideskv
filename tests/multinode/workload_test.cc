#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "multinode/multinode_helpers.h"
#include "multinode/workload.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// F6: the load accounts for what it could not do, so a test whose server stays up can tell a healthy
// load from a failed one.
namespace tideskv::test {
namespace {

using namespace multinode;

constexpr int kClients = 2;

TEST(Workload, CountsErrorRepliesByTextAndClientsThatEndEarly) {
  // A replica of nothing: up, read-only, so every write is refused.
  TestServer replica(TIDESKV_SERVER_BIN, {.extra_args = {"--replicaof", "127.0.0.1 1"}});
  Workload load(replica.port(), Seed(), kClients);
  load.Start();
  std::this_thread::sleep_for(300ms);
  load.Stop();
  ReportLoad(load);
  Workload::Stats st = load.stats();
  EXPECT_GT(st.steps, 0u);
  EXPECT_EQ(st.early_exits, 0u);
  EXPECT_GT(st.errors["READONLY You can't write against a read only replica."], 0u);
  EXPECT_GT(st.error_count(), st.steps / 2);
  replica.Kill9();
  load.Start();  // nothing to connect to: every client ends at once
  std::this_thread::sleep_for(100ms);
  load.Stop();
  EXPECT_EQ(load.stats().early_exits, static_cast<uint64_t>(kClients));
}

}  // namespace
}  // namespace tideskv::test
