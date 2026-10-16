#pragma once
#include <cstdint>

// The calling thread's local commits (M6b D9, M7a D24): what WAIT counts ACKs against. Dispatch and
// BlockedClient::TryServe compare the commit COUNT before and after running a command to learn whether it
// wrote, then read the seq of its last commit. Comparing seqs missed a write whose seq equalled a stale
// value this thread kept from an earlier ReplLog (another test, a node promoted after a full sync).
namespace tideskv {
namespace engine_internal {
inline thread_local uint64_t t_last_commit_seq = 0;  // set by ReplLog::Commit (local commits only)
inline thread_local uint64_t t_commit_count = 0;     // incremented by ReplLog::Commit with the seq
}  // namespace engine_internal
inline uint64_t ThreadLastCommitSeq() { return engine_internal::t_last_commit_seq; }
inline uint64_t ThreadCommitCount() { return engine_internal::t_commit_count; }
}  // namespace tideskv
