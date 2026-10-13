#include "commands/reply_util.h"

#include <spdlog/spdlog.h>

namespace tideskv {

std::string InvalidExpireError(std::string_view command) {
  return "ERR invalid expire time in '" + std::string(command) + "' command";
}

std::string ArityError(std::string_view command) {
  return "ERR wrong number of arguments for '" + std::string(command) + "' command";
}

std::string AtLeastOneKeyError(std::string_view command) {
  return "ERR at least 1 input key is needed for '" + std::string(command) + "' command";
}

std::string UnsupportedOptionError(std::string_view opt) {
  return "ERR Unsupported option " + std::string(opt);
}

void ReplyError(ExecContext& ctx, const Status& st) {
  switch (st.code()) {
    case StatusCode::kUserError:
      return ctx.reply.Error(st.message());
    case StatusCode::kBusy:
      return ctx.reply.Error("ERR storage busy");
    case StatusCode::kConflict:
      return ctx.reply.Error("ERR conflict");
    case StatusCode::kNoSpace:
      spdlog::error("write failed, disk full: {}", st.message());
      return ctx.reply.Error("ERR disk full, writes disabled");
    default:
      spdlog::error("command failed for client id={}: {}", ctx.session.id, st.ToString());
      return ctx.reply.Error("ERR storage error");
  }
}

}  // namespace tideskv
