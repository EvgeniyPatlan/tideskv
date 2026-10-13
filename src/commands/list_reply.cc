#include "commands/list_reply.h"

#include <string>

#include "common/string_util.h"
#include "resp/reply_builder.h"
#include "types/errors.h"

namespace tideskv {

Status ParseListEndArg(std::string_view arg, ListEnd* out) {
  if (EqualsIgnoreCase(arg, "left")) {
    *out = ListEnd::kLeft;
  } else if (EqualsIgnoreCase(arg, "right")) {
    *out = ListEnd::kRight;
  } else {
    return Status::UserError(std::string(kErrSyntax));
  }
  return Status::Ok();
}

void ReplyPopped(ExecContext& ctx, const std::optional<PoppedFrom>& popped) {
  if (!popped) return ctx.reply.NullArray();
  ctx.reply.ArrayHeader(2);
  ctx.reply.Bulk(popped->key);
  ctx.reply.ArrayHeader(popped->values.size());
  for (const std::string& v : popped->values) ctx.reply.Bulk(v);
}

}  // namespace tideskv
