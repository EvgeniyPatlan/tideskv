#pragma once
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "repl/full_sync_frames.h"

namespace tideskv::test {

// What a master sends after +FULLRESYNC <replid> <seq> (M6b D4): the snapshot's LOAD frames, then
// LOADEND <seq> <replog[seq]> <ops> (an empty record at seq 0).
struct SnapshotStream {
  uint64_t seq = 0;
  std::vector<std::vector<std::string>> frames;  // {"LOAD", chunk}..., {"LOADEND", seq, record, ops}

  // The frames as the master writes them on the link.
  std::string Wire() const {
    std::string out;
    for (const auto& f : frames) {
      out += f[0] == "LOAD" ? repl::LoadFrame(f[1]) : repl::LoadEndFrame(seq, f[2], std::stoull(f[3]));
    }
    return out;
  }
};

// The stream of `log`'s snapshot at its last seq, as Task 7's master produces it (chunk timestamp 1).
inline SnapshotStream SnapshotFrames(ReplLog& log) {
  SnapshotStream s;
  auto snap = log.ActivateAndSnapshot();
  EXPECT_TRUE(snap.ok()) << snap.status().ToString();
  if (!snap.ok()) return s;
  s.seq = snap->seq;
  repl::SnapshotChunker chunker(*snap->view, 1);
  for (;;) {
    auto chunk = chunker.Next(nullptr);
    EXPECT_TRUE(chunk.ok()) << chunk.status().ToString();
    if (!chunk.ok() || !chunk->has_value()) break;
    s.frames.push_back({"LOAD", **chunk});
  }
  std::string record;
  if (s.seq != 0) {
    auto r = snap->view->Get(ColumnFamily::kReplog, encoding::ReplogKey(s.seq));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    if (r.ok()) record = *r;
  }
  s.frames.push_back({"LOADEND", std::to_string(s.seq), record, std::to_string(chunker.ops())});
  return s;
}

}  // namespace tideskv::test
