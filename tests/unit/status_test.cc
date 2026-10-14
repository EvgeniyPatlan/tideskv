#include "common/status.h"

#include <gtest/gtest.h>

#include <memory>

namespace tideskv {
namespace {

TEST(Status, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(Status, FactoriesSetCodeAndMessage) {
  EXPECT_EQ(Status::NotFound("k").code(), StatusCode::kNotFound);
  EXPECT_TRUE(Status::NotFound().IsNotFound());
  EXPECT_EQ(Status::InvalidArgument("x").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Status::Busy("x").code(), StatusCode::kBusy);
  EXPECT_EQ(Status::Conflict("x").code(), StatusCode::kConflict);
  EXPECT_EQ(Status::NoSpace("x").code(), StatusCode::kNoSpace);
  EXPECT_EQ(Status::IOError("x").code(), StatusCode::kIOError);
  EXPECT_EQ(Status::Corruption("x").code(), StatusCode::kCorruption);
  EXPECT_EQ(Status::Internal("x").code(), StatusCode::kInternal);
  EXPECT_EQ(Status::IOError("disk").message(), "disk");
  EXPECT_FALSE(Status::IOError("disk").ok());
}

TEST(Status, ToStringIncludesCodeAndMessage) {
  EXPECT_EQ(Status::Corruption("bad block").ToString(), "Corruption: bad block");
  EXPECT_EQ(Status::NotFound().ToString(), "NotFound");
}

// GCC -O2 emits a known -Wmaybe-uninitialized false positive from the inlined
// std::variant<int, Status> destructor when the active alternative is the int (the
// std::string inside the inactive Status alternative is reported as uninitialized).
// Result is correct (see HoldsError); suppress narrowly for this test only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
TEST(Result, HoldsValue) {
  Result<int> r(42);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, 42);
  EXPECT_TRUE(r.status().ok());
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

TEST(Result, HoldsError) {
  Result<int> r(Status::NotFound("missing"));
  ASSERT_FALSE(r.ok());
  EXPECT_TRUE(r.status().IsNotFound());
  EXPECT_EQ(r.status().message(), "missing");
}

TEST(Result, SupportsMoveOnlyTypes) {
  Result<std::unique_ptr<int>> r(std::make_unique<int>(7));
  ASSERT_TRUE(r.ok());
  std::unique_ptr<int> p = std::move(r).value();
  EXPECT_EQ(*p, 7);
}

TEST(Status, UserErrorCarriesTheReplyText) {
  const Status st = Status::UserError("WRONGTYPE Operation against a key holding the wrong kind of value");
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), StatusCode::kUserError);
  EXPECT_EQ(st.message(), "WRONGTYPE Operation against a key holding the wrong kind of value");
  EXPECT_EQ(StatusCodeName(StatusCode::kUserError), "UserError");
}

TEST(Status, TimedOut) {
  EXPECT_EQ(Status::TimedOut().code(), StatusCode::kTimedOut);
  EXPECT_EQ(Status::TimedOut("no data").ToString(), "TimedOut: no data");
}

}  // namespace
}  // namespace tideskv
