#pragma once
#include <cstdint>

// The seq of the calling thread's last local commit (M6b D9): what WAIT counts ACKs against. Dispatch and
// BlockedClient::TryServe compare it before and after running a command to learn whether it wrote.
namespace tideskv {
namespace engine_internal {
inline thread_local uint64_t t_last_commit_seq = 0;  // set by ReplLog::Commit (local commits only)
}  // namespace engine_internal
inline uint64_t ThreadLastCommitSeq() { return engine_internal::t_last_commit_seq; }
}  // namespace tideskv
