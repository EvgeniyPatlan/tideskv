#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "encoding/key_layout.h"
#include "exec/engine.h"
#include "repl/repl_state.h"

namespace tideskv::repl {

// The replica side of one full synchronisation (M6b D4, D5): fed the frames that follow
// +FULLRESYNC <replid> <seq>, it loads the master's snapshot through the Engine's load calls. Destroying
// it after Begin() and before LOADEND committed aborts the load (the keyspace is wiped, D5). It never
// applies a BATCH (CF-M6b-4): the caller streams only once HandleFrame reported done.
class FullSyncLoader {
 public:
  struct Hooks {
    std::function<ReplIdentity()> identity;                     // required: the node's current identity
    std::function<void(const ReplIdentity&)> publish_identity;  // required: persisted by the load txns
    std::function<void()> on_baseline_known;                    // optional: the start txn cleared R13's flag
    std::function<int()> key_load_delay_us;                     // optional: key-load-delay (D14)
    std::function<bool()> stopping;                             // optional: cuts the key-load-delay short
  };
  // adopt: the master's key layout when it is not this node's; the load's start txn then writes its
  // format_version with the wipe, and the process switches to it under the Engine's load barrier.
  FullSyncLoader(Engine& engine, std::string master_replid, uint64_t seq, Hooks hooks,
                 std::optional<encoding::KeyLayout> adopt = std::nullopt);
  ~FullSyncLoader();
  FullSyncLoader(const FullSyncLoader&) = delete;
  FullSyncLoader& operator=(const FullSyncLoader&) = delete;
  Status Begin();  // Engine::BeginLoad with this node's replid and no history
  // One frame: PING, LOAD <chunk> or LOADEND <seq> <record> <ops>; *done once LOADEND committed.
  // IOError("protocol error: ...") for anything else, and for any frame once done; Corruption for a
  // chunk that is not a snapshot batch. The caller drops the link.
  Status HandleFrame(const std::vector<std::string>& args, bool* done);
  uint64_t loaded_bytes() const { return loaded_bytes_; }

 private:
  Status Chunk(std::string_view chunk);
  Status End(const std::vector<std::string>& args, bool* done);
  void DelayPerKey(size_t keys) const;

  Engine& engine_;
  std::string master_replid_;
  uint64_t seq_;
  Hooks hooks_;
  std::optional<encoding::KeyLayout> adopt_;
  bool begun_ = false;
  bool finished_ = false;
  uint64_t ops_ = 0;
  uint64_t loaded_bytes_ = 0;
};

}  // namespace tideskv::repl
