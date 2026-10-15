#include "repl/full_sync_loader.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "common/debug_delay.h"
#include "common/string_util.h"
#include "encoding/replog_codec.h"
#include "repl/full_sync_frames.h"

namespace tideskv::repl {

FullSyncLoader::FullSyncLoader(Engine& engine, std::string master_replid, uint64_t seq, Hooks hooks)
    : engine_(engine), master_replid_(std::move(master_replid)), seq_(seq), hooks_(std::move(hooks)) {}

FullSyncLoader::~FullSyncLoader() {
  if (!begun_ || finished_) return;
  if (Status st = engine_.AbortLoad(); !st.ok()) {
    spdlog::error("cannot discard a partial full-sync load: {}", st.ToString());  // startup retries (marker)
  }
}

Status FullSyncLoader::Begin() {
  const ReplIdentity cleared{hooks_.identity().replid, {}};  // the old history no longer describes the data
  if (Status st = engine_.BeginLoad(IdentityOps(cleared)); !st.ok()) return st;
  begun_ = true;
  hooks_.publish_identity(cleared);
  if (hooks_.on_baseline_known) hooks_.on_baseline_known();
  return Status::Ok();
}

Status FullSyncLoader::HandleFrame(const std::vector<std::string>& args, bool* done) {
  if (finished_) return Status::IOError("protocol error: a frame after the full synchronisation ended");
  if (args.size() == 1 && EqualsIgnoreCase(args[0], "ping")) return Status::Ok();
  if (args.size() == 2 && EqualsIgnoreCase(args[0], "load")) return Chunk(args[1]);
  if (args.size() == 4 && EqualsIgnoreCase(args[0], "loadend")) return End(args, done);
  return Status::IOError("protocol error: unexpected frame during a full synchronisation");
}

Status FullSyncLoader::Chunk(std::string_view chunk) {
  // CF-M6b-6: a master encodes every chunk as a standalone batch (no stream bytes before it), so its
  // cum_bytes is exactly its own size; anything else is not a snapshot chunk.
  uint64_t cum_bytes = 0;
  int64_t timestamp_ms = 0;
  if (encoding::PeekReplogHeader(chunk, &timestamp_ms, &cum_bytes) && cum_bytes != chunk.size()) {
    return Status::Corruption("full-sync chunk: a stream offset in a snapshot batch");
  }
  auto ops = DecodeLoadChunk(chunk);
  if (!ops.ok()) return ops.status();
  if (Status st = engine_.LoadChunk(*ops); !st.ok()) return st;
  ops_ += ops->size();
  loaded_bytes_ += chunk.size();
  DelayPerKey(ops->size());
  return Status::Ok();
}

// key-load-delay (D14), per key as Redis; a stop request ends it (PF14), the caller then drops the link.
void FullSyncLoader::DelayPerKey(size_t keys) const {
  const int delay = hooks_.key_load_delay_us ? hooks_.key_load_delay_us() : 0;
  for (size_t i = 0; delay != 0 && i < keys; ++i) {
    if (hooks_.stopping && hooks_.stopping()) return;
    DebugDelay(delay);
  }
}

Status FullSyncLoader::End(const std::vector<std::string>& args, bool* done) {
  uint64_t seq = 0;
  uint64_t ops = 0;
  if (!ParseUint64(args[1], &seq) || !ParseUint64(args[3], &ops)) {
    return Status::IOError("protocol error: malformed LOADEND");
  }
  if (seq != seq_ || ops != ops_) {
    return Status::IOError("protocol error: LOADEND " + args[1] + "/" + args[3] + " does not match seq " +
                           std::to_string(seq_) + " and " + std::to_string(ops_) + " loaded ops");
  }
  const ReplIdentity adopted{master_replid_, {}};  // Redis clears replid2 after a full sync
  if (Status st = engine_.EndLoad(seq, args[2], IdentityOps(adopted)); !st.ok()) return st;
  finished_ = true;
  hooks_.publish_identity(adopted);
  *done = true;
  return Status::Ok();
}

}  // namespace tideskv::repl
