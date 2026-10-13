#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "commands/registry.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/status.h"
#include "exec/engine.h"
#include "server/blocking.h"
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

// After a successful write that may have created a list or zset at `key` (M4 plan F6).
inline void SignalReady(ExecContext& ctx, std::string_view key) {
  if (ctx.ready == nullptr || !ctx.server.blocking().HasWaiters(ctx.session.db, key)) return;
  ctx.ready->push_back({ctx.session.db, std::string(key)});
}

// Runs fn(WriteTxn&, int64_t now_ms) -> Status in one Engine::Write that locks `key`;
// now_ms is the Engine's under-lock clock (see Engine). Returns true on success (the
// caller then replies); on failure replies the error and returns false. fn runs again
// after a commit conflict, so it must reset its outputs.
template <typename Fn>
bool WriteOneKey(ExecContext& ctx, std::string_view key, Fn&& fn,
                 std::vector<std::string>* also_modified = nullptr) {
  const Status st = ctx.server.engine().Write(ctx.session.db, {key}, fn, also_modified);
  if (st.ok()) return true;
  ReplyError(ctx, st);
  return false;
}

// Replies a bulk string, or nil for nullopt.
inline void ReplyOptional(ExecContext& ctx, const std::optional<std::string>& v) {
  v ? ctx.reply.Bulk(*v) : ctx.reply.Null();
}

// argv[first, last) as views (keys, members, values ...).
inline std::vector<std::string_view> ArgsFrom(const ArgList& argv, size_t first, size_t last) {
  return std::vector<std::string_view>(argv.begin() + static_cast<std::ptrdiff_t>(first),
                                       argv.begin() + static_cast<std::ptrdiff_t>(last));
}
inline std::vector<std::string_view> ArgsFrom(const ArgList& argv, size_t first) {
  return ArgsFrom(argv, first, argv.size());
}

// Runs fn(ReadView&, int64_t now_ms) -> Status through Engine::Read (Engine::Snapshot for
// RunSnapshot). Returns true on success; otherwise replies the error and returns false.
template <typename Fn>
bool RunRead(ExecContext& ctx, Fn&& fn) {
  const Status st = ctx.server.engine().Read(fn);
  if (st.ok()) return true;
  ReplyError(ctx, st);
  return false;
}

template <typename Fn>
bool RunSnapshot(ExecContext& ctx, Fn&& fn) {
  const Status st = ctx.server.engine().Snapshot(fn);
  if (st.ok()) return true;
  ReplyError(ctx, st);
  return false;
}

}  // namespace tideskv
