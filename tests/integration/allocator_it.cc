#include <gtest/gtest.h>

#include "support/it_helpers.h"
#include "support/test_server.h"

namespace tideskv::test {
namespace {

TEST(AllocatorIT, InfoReportsAllocator) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  EXPECT_EQ(InfoString(*c, "server", "mem_allocator"), TIDESKV_EXPECTED_ALLOCATOR);
}

}  // namespace
}  // namespace tideskv::test
