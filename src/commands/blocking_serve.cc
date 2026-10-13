#include "commands/blocking_serve.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <string>

#include "commands/server_services.h"
#include "exec/engine.h"
#include "server/blocking.h"
#include "types/object_store.h"

namespace tideskv {
namespace {

// Makes a re-run command report "would block" instead of parking again.
class ServeBlockHandler final : public BlockHandler {
 public:
  void Block(const BlockRequest&) override { would_block = true; }
  bool would_block = false;
};

std::optional<std::string> ReRun(const CommandRegistry& registry, ServerServices& server,
                                 const BlockedClient& w, ReadyKeys& ready) {
  const CommandSpec* spec = registry.Find(w.argv().front());
  if (spec == nullptr) return std::nullopt;  // impossible: it was found when the client blocked
  Session session = w.session();
  resp::ReplyBuilder reply(session.protocol);
  ServeBlockHandler block;
  ExecContext ctx{session, reply, server, &block, &ready};
  spec->handler(ctx, w.argv());
  if (block.would_block) return std::nullopt;
  return reply.Take();
}

// The live type of (db, key), or nullopt when it is missing or expired.
Result<std::optional<encoding::ValueType>> LiveType(Engine& engine, uint8_t db, std::string_view key) {
  std::optional<encoding::ValueType> type;
  const Status st = engine.Read([&](ReadView& view, int64_t now_ms) -> Status {
    auto obj = LoadObject(view, db, key, now_ms);
    if (!obj.ok()) return obj.status();
    if (obj->live()) type = obj->meta.type;
    return Status::Ok();
  });
  if (!st.ok()) return st;
  return type;
}

void ServeKey(const CommandRegistry& registry, ServerServices& server, const ReadyKey& rk, ReadyKeys& ready) {
  for (const auto& waiter : server.blocking().WaitersOn(rk.db, rk.key)) {
    auto type = LiveType(server.engine(), rk.db, rk.key);
    if (!type.ok()) {
      spdlog::error("serving blocked clients: {}", type.status().ToString());
      return;
    }
    if (!*type) return;                      // the key is gone: nobody can be served from it
    if (**type != waiter->type()) continue;  // e.g. a BLPOP waiter on a key that became a zset
    const auto result = waiter->TryServe([&] { return ReRun(registry, server, *waiter, ready); });
    if (result == BlockedClient::ServeResult::kStillBlocked) return;  // the key is empty again
  }
}

}  // namespace

void ServeBlockedClients(const CommandRegistry& registry, ServerServices& server, ReadyKeys& ready) {
  // Index loop: serving may append (BLMOVE destinations).
  for (size_t i = 0; i < ready.size(); ++i) {
    const ReadyKey rk = ready[i];  // copy: `ready` may reallocate while serving
    ServeKey(registry, server, rk, ready);
  }
  ready.clear();
}

}  // namespace tideskv
