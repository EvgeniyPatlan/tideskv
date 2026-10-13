#include "commands/multi_state.h"

#include "commands/server_services.h"
#include "server/watch.h"

namespace tideskv {

ConnectionState ConnectionState::Capture(const Session& s) {
  return ConnectionState{s.db, s.protocol, s.authenticated, s.name};
}

void ConnectionState::Restore(Session& s, resp::ReplyBuilder& reply) const {
  s.db = db;
  s.protocol = protocol;
  s.authenticated = authenticated;
  s.name = name;
  reply.SetProtocol(protocol);
}

void UnwatchClient(ExecContext& ctx) {
  if (ctx.session.watch) ctx.server.watches().UnwatchAll(*ctx.session.watch);
}

void DiscardTransaction(ExecContext& ctx) {
  ctx.session.multi = MultiState{};
  UnwatchClient(ctx);
}

}  // namespace tideskv
