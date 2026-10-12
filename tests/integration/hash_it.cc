#include <gtest/gtest.h>

#include <set>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;

TEST(HashIT, BigValuesAndResp3Map) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  const std::string big(1 << 20, 'v');
  EXPECT_EQ(c->Command({"HSET", "h", "big", big, "small", "x"}).integer, 2);
  EXPECT_EQ(c->Command({"HGET", "h", "big"}).str, big);
  EXPECT_EQ(c->Command({"HSTRLEN", "h", "big"}).integer, 1 << 20);
  ASSERT_EQ(c->Command({"HELLO", "3"}).type, Type::kMap);
  const RespValue all = c->Command({"HGETALL", "h"});
  ASSERT_EQ(all.type, Type::kMap);
  ASSERT_EQ(all.elements.size(), 4u);
  EXPECT_EQ(all.elements[0].str, "big");
}

TEST(HashIT, HscanVisitsAThousandFields) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 1000; i += 100) {
    std::vector<std::string> argv = {"HSET", "h"};
    for (int j = i; j < i + 100; ++j) {
      argv.push_back("f" + std::to_string(j));
      argv.push_back(std::to_string(j));
    }
    ASSERT_EQ(c->Command(argv).integer, 100);
  }
  std::set<std::string> fields;
  std::string cursor = "0";
  int calls = 0;
  do {
    const RespValue r = c->Command({"HSCAN", "h", cursor, "COUNT", "37"});
    ASSERT_EQ(r.elements.size(), 2u);
    cursor = r.elements[0].str;
    for (size_t i = 0; i < r.elements[1].elements.size(); i += 2)
      fields.insert(r.elements[1].elements[i].str);
  } while (cursor != "0" && ++calls < 1000);
  EXPECT_EQ(fields.size(), 1000u);
}

TEST(HashIT, CrossSlotRenameOfALargeHashOnTheRealBackend) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 3000; i += 100) {
    std::vector<std::string> argv = {"HSET", "src"};
    for (int j = i; j < i + 100; ++j) {
      argv.push_back("f" + std::to_string(j));
      argv.push_back(std::to_string(j));
    }
    ASSERT_EQ(c->Command(argv).integer, 100);
  }
  EXPECT_EQ(c->Command({"RENAME", "src", "dst"}).str, "OK");
  EXPECT_EQ(c->Command({"HLEN", "dst"}).integer, 3000);
  EXPECT_EQ(c->Command({"HGET", "dst", "f2999"}).str, "2999");
  EXPECT_EQ(c->Command({"EXISTS", "src"}).integer, 0);
}

}  // namespace
}  // namespace tideskv::test
