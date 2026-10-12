#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "commands/registry.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/status.h"
#include "exec/engine.h"
#include "types/string_type.h"

// Engine boilerplate shared by the data-command handlers. All helpers act on the session's db.
namespace tideskv {

// Reads the string at `key`: *out is nullopt when the key is missing or expired; a
// non-string key gives UserError(WRONGTYPE). The caller replies.
inline Status ReadStringValue(ExecContext& ctx, std::string_view key, std::optional<std::string>* out) {
  Engine& engine = ctx.server.engine();
  const int64_t now = engine.NowMs();
  return engine.Read(
      [&](ReadView& view) { return MoveInto(StringGet(view, ctx.session.db, key, now), out); });
}

// Runs fn(WriteTxn&, int64_t now_ms) -> Status in one Engine::Write that locks `key`.
// Returns true on success (the caller then replies); on failure replies the error and
// returns false. fn runs again after a commit conflict, so it must reset its outputs.
template <typename Fn>
bool WriteOneKey(ExecContext& ctx, std::string_view key, int64_t now_ms, Fn&& fn) {
  const Status st =
      ctx.server.engine().Write(ctx.session.db, {key}, [&](WriteTxn& txn) { return fn(txn, now_ms); });
  if (st.ok()) return true;
  ReplyError(ctx, st);
  return false;
}

// As above, with now_ms read from the engine clock.
template <typename Fn>
bool WriteOneKey(ExecContext& ctx, std::string_view key, Fn&& fn) {
  return WriteOneKey(ctx, key, ctx.server.engine().NowMs(), fn);
}

}  // namespace tideskv
