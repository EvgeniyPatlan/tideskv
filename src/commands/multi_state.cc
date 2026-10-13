#include "commands/multi_state.h"

#include "commands/server_services.h"
#include "server/watch.h"

namespace tideskv {

void UnwatchClient(ExecContext& ctx) {
  if (ctx.session.watch) ctx.server.watches().UnwatchAll(*ctx.session.watch);
}

void DiscardTransaction(ExecContext& ctx) {
  ctx.session.multi = MultiState{};
  UnwatchClient(ctx);
}

}  // namespace tideskv
