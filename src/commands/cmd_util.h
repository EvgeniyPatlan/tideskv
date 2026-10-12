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
  return ctx.server.engine().Read([&](ReadView& view, int64_t now_ms) {
    return MoveInto(StringGet(view, ctx.session.db, key, now_ms), out);
  });
}

// Runs fn(WriteTxn&, int64_t now_ms) -> Status in one Engine::Write that locks `key`;
// now_ms is the Engine's under-lock clock (see Engine). Returns true on success (the
// caller then replies); on failure replies the error and returns false. fn runs again
// after a commit conflict, so it must reset its outputs.
template <typename Fn>
bool WriteOneKey(ExecContext& ctx, std::string_view key, Fn&& fn) {
  const Status st = ctx.server.engine().Write(ctx.session.db, {key}, fn);
  if (st.ok()) return true;
  ReplyError(ctx, st);
  return false;
}

}  // namespace tideskv
