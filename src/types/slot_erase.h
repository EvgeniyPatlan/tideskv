#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "encoding/replog_codec.h"
#include "storage/storage.h"

// What erasing a slot range of db 0 writes (M7b D9), as replog ops; migrate::SlotEraser commits them through
// Engine::WriteInternal. Pure: no Engine here (types/ never includes exec/, Writer's note 3).
namespace tideskv {

struct ExpireDeletes {
  std::vector<encoding::ReplOp> ops;  // DEL of the expire entry of each walked meta with a TTL
  std::string resume_after;           // the last meta key walked; the next call starts after it
  bool done = false;                  // the walk reached the end of the range
};

// Step 1: walks the metas of slots first..last after `after` ("" = from the first) and collects the expire
// deletes of the metas with a TTL among at most `limit` metas walked (bounds the shared-gate hold), each from
// the meta's own deadline (as DeleteObject). Logically expired, unreaped keys count: they are stored state.
Result<ExpireDeletes> CollectExpireDeletes(ReadView& view, uint16_t first, uint16_t last,
                                           std::string_view after, size_t limit);
// Step 2: DeleteRange([0][first], [0][last+1]) in meta, subkey and zscore. Three range deletes stay under
// TidesDB's 32-per-commit cap (storage.h); one DeletePrefix per slot would not.
std::vector<encoding::ReplOp> SlotRangeDeletes(uint16_t first, uint16_t last);

}  // namespace tideskv
