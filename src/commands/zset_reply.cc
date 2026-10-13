#include "commands/zset_reply.h"

namespace tideskv {

void ReplyScored(ExecContext& ctx, const std::vector<ScoreMember>& items, bool with_scores) {
  if (!with_scores) {
    ctx.reply.ArrayHeader(items.size());
    for (const ScoreMember& p : items) ctx.reply.Bulk(p.member);
    return;
  }
  if (ctx.reply.protocol() >= 3) {
    ctx.reply.ArrayHeader(items.size());
    for (const ScoreMember& p : items) {
      ctx.reply.ArrayHeader(2);
      ctx.reply.Bulk(p.member);
      ctx.reply.Double(p.score);
    }
    return;
  }
  ctx.reply.ArrayHeader(items.size() * 2);
  for (const ScoreMember& p : items) {
    ctx.reply.Bulk(p.member);
    ctx.reply.Double(p.score);
  }
}

}  // namespace tideskv
