#include "commands/reply_util.h"

#include <gtest/gtest.h>

#include "support/command_harness.h"

namespace tideskv {
namespace {

std::string ErrorFor(const Status& st) {
  test::CommandHarness h;
  ExecContext ctx{h.session, h.reply, h.services};
  ReplyError(ctx, st);
  return h.reply.Take();
}

TEST(ReplyUtil, UserErrorIsSentVerbatim) {
  EXPECT_EQ(ErrorFor(Status::UserError(std::string(kErrWrongType))),
            "-" + std::string(kErrWrongType) + "\r\n");
}

TEST(ReplyUtil, StorageErrorsMapPerSpecAndHideDetails) {
  EXPECT_EQ(ErrorFor(Status::Busy("locked /data/db")), "-ERR storage busy\r\n");
  EXPECT_EQ(ErrorFor(Status::Conflict("x")), "-ERR conflict\r\n");
  EXPECT_EQ(ErrorFor(Status::NoSpace("x")), "-ERR disk full, writes disabled\r\n");
  EXPECT_EQ(ErrorFor(Status::IOError("/secret/path: tidesdb error -7")), "-ERR storage error\r\n");
  EXPECT_EQ(ErrorFor(Status::Corruption("x")), "-ERR storage error\r\n");
  EXPECT_EQ(ErrorFor(Status::Internal("x")), "-ERR storage error\r\n");
}

TEST(ReplyUtil, MessageHelpers) {
  EXPECT_EQ(InvalidExpireError("set"), "ERR invalid expire time in 'set' command");
  EXPECT_EQ(ArityError("mset"), "ERR wrong number of arguments for 'mset' command");
  EXPECT_EQ(UnsupportedOptionError("KEEPTTL"), "ERR Unsupported option KEEPTTL");
}

}  // namespace
}  // namespace tideskv
