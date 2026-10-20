#include "repl/repl_sender.h"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "repl/full_sync_frames.h"
#include "repl/socket_io.h"
#include "resp/request_parser.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
using encoding::ReplOp;
using encoding::ReplOpType;

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return cond();
}

// The replica's end of the link: parses BATCH and PING frames with the server's own request parser.
class Reader {
 public:
  explicit Reader(Fd fd) : fd_(std::move(fd)) {}

  // Reads frames until `n` have arrived or the timeout passes.
  std::vector<std::vector<std::string>> Frames(size_t n, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (frames_.size() < n && std::chrono::steady_clock::now() < deadline) {
      if (!Pump(20ms)) break;
    }
    return frames_;
  }
  // Reads for `d` and returns every frame seen so far.
  std::vector<std::vector<std::string>> For(std::chrono::milliseconds d) {
    const auto deadline = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!Pump(10ms)) break;
    }
    return frames_;
  }
  void Send(std::string_view bytes) { ASSERT_TRUE(WriteAll(fd_.get(), bytes, 1s).ok()); }
  void Close() { fd_ = Fd(); }
  int fd() const { return fd_.get(); }

 private:
  bool Pump(std::chrono::milliseconds wait) {
    const Status st = ReadSome(fd_.get(), &in_, wait);
    if (!st.ok() && st.code() != StatusCode::kTimedOut) return false;
    size_t off = 0;
    for (;;) {
      size_t consumed = 0;
      const auto ps = parser_.Parse(std::string_view(in_).substr(off), &consumed);
      off += consumed;
      if (ps != resp::ParseStatus::kCommand) break;
      frames_.push_back(parser_.args());
    }
    in_.erase(0, off);
    return true;
  }

  Fd fd_;
  std::string in_;
  resp::RequestParser parser_;
  std::vector<std::vector<std::string>> frames_;
};

std::vector<ReplOp> MetaPut(const std::string& key) {
  return {{ReplOpType::kPut, ColumnFamily::kMeta, key, "v"}};
}

struct SenderTest : ::testing::Test {
  SenderTest() { EXPECT_TRUE(log.Recover().ok()); }

  void Commit(int n) {
    for (int i = 0; i < n; ++i) ASSERT_TRUE(log.CommitStandalone(MetaPut("k" + std::to_string(++keys))).ok());
  }
  std::string ReplogAt(uint64_t seq) {
    auto view = storage.BeginRead();
    auto v = (*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(seq));
    return v.ok() ? *v : std::string();
  }
  // Starts a sender at next_seq over a fresh socketpair; the test keeps the other end.
  void StartSender(uint64_t next_seq, ReplSender::Callbacks cb = {}) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    reader = std::make_unique<Reader>(Fd(fds[1]));
    sender = std::make_unique<ReplSender>(storage, log, Fd(fds[0]), next_seq, std::move(cb));
    sender->Start();
  }

  InMemoryStorage storage;
  test::FakeClock clock;
  ReplLog log{storage, clock};
  int keys = 0;
  std::unique_ptr<Reader> reader;
  std::unique_ptr<ReplSender> sender;
};

std::vector<std::string> Batch(uint64_t seq, const std::string& payload) {
  return {"BATCH", std::to_string(seq), payload};
}

TEST(ReplSenderFrame, BatchAndPingFramesAreRespArrays) {
  EXPECT_EQ(BatchFrame(12, "ab"), "*3\r\n$5\r\nBATCH\r\n$2\r\n12\r\n$2\r\nab\r\n");
  EXPECT_EQ(kPingFrame, "*1\r\n$4\r\nPING\r\n");
}

TEST_F(SenderTest, StreamsExistingBatchesFromNextSeq) {
  Commit(5);
  StartSender(3);
  const auto frames = reader->Frames(3);
  ASSERT_EQ(frames.size(), 3u);
  for (uint64_t seq = 3; seq <= 5; ++seq) EXPECT_EQ(frames[seq - 3], Batch(seq, ReplogAt(seq)));
  EXPECT_TRUE(sender->alive());
}

TEST_F(SenderTest, StreamsLiveCommitsPromptly) {
  Commit(1);
  StartSender(2);
  std::this_thread::sleep_for(50ms);  // the sender is now waiting on the watermark
  const auto start = std::chrono::steady_clock::now();
  Commit(1);
  const auto frames = reader->Frames(1, 1s);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0], Batch(2, ReplogAt(2)));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
}

TEST_F(SenderTest, StartsFromAnEmptyLog) {
  StartSender(1);
  Commit(2);
  const auto frames = reader->Frames(2);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[1], Batch(2, ReplogAt(2)));
}

TEST_F(SenderTest, CoalescesALargeBacklogAcrossWrites) {
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(log.CommitStandalone({{ReplOpType::kPut, ColumnFamily::kMeta, "k" + std::to_string(i),
                                       std::string(4096, 'v')}})
                    .ok());
  }
  StartSender(1);
  const auto frames = reader->Frames(300, 5s);
  ASSERT_EQ(frames.size(), 300u);
  for (uint64_t seq = 1; seq <= 300; ++seq) ASSERT_EQ(frames[seq - 1][1], std::to_string(seq));
  EXPECT_EQ(frames[299], Batch(300, ReplogAt(300)));
}

TEST_F(SenderTest, SendsPingWhenIdle) {
  StartSender(1);
  const auto frames = reader->Frames(1, 1500ms);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0], std::vector<std::string>{"PING"});
}

TEST_F(SenderTest, ParsesAcks) {
  Commit(5);
  StartSender(1);
  ASSERT_EQ(reader->Frames(5).size(), 5u);
  EXPECT_EQ(sender->acked(), 0u);
  const int64_t before = sender->last_ack_ms();
  std::this_thread::sleep_for(20ms);
  reader->Send(RespCommand({"REPLCONF", "ACK", "4"}));
  ASSERT_TRUE(WaitFor([&] { return sender->acked() == 4; }, 1s));
  EXPECT_GT(sender->last_ack_ms(), before);
  // Split across reads, lowercase, and pipelined with a second ACK.
  const std::string two = RespCommand({"replconf", "ack", "5"}) + RespCommand({"REPLCONF", "ACK", "5"});
  reader->Send(two.substr(0, 7));
  std::this_thread::sleep_for(30ms);
  reader->Send(two.substr(7));
  ASSERT_TRUE(WaitFor([&] { return sender->acked() == 5; }, 1s));
  EXPECT_TRUE(sender->alive());
}

TEST_F(SenderTest, UnreadBytesFromTheHandoffAreParsedFirst) {
  Commit(3);
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  reader = std::make_unique<Reader>(Fd(fds[1]));
  sender = std::make_unique<ReplSender>(storage, log, Fd(fds[0]), 3, ReplSender::Callbacks{});
  sender->SetUnread(RespCommand({"REPLCONF", "ACK", "2"}));
  sender->SetPeer("10.0.0.7", 7001);
  sender->Start();
  ASSERT_TRUE(WaitFor([&] { return sender->acked() == 2; }, 1s));
  EXPECT_EQ(sender->peer_ip(), "10.0.0.7");
  EXPECT_EQ(sender->listening_port(), 7001);
}

TEST_F(SenderTest, DropsTheLinkOnPeerClose) {
  StartSender(1);
  reader->Close();
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 1s));
  const auto start = std::chrono::steady_clock::now();
  sender->Stop();  // the thread already exited: the join is immediate
  EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
}

TEST_F(SenderTest, DropsTheLinkOnAProtocolError) {
  StartSender(1);
  reader->Send("*1\r\n$x\r\n");
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 1s));
}

TEST_F(SenderTest, DropsTheLinkOnAnUnexpectedCommand) {
  StartSender(1);
  reader->Send(RespCommand({"SET", "k", "v"}));
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 1s));
}

TEST_F(SenderTest, DropsTheLinkOnABadAck) {
  Commit(2);
  StartSender(1);
  ASSERT_EQ(reader->Frames(2).size(), 2u);
  reader->Send(RespCommand({"REPLCONF", "ACK", "nope"}));
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 1s));
}

TEST_F(SenderTest, DropsTheLinkOnAnAckBeyondWhatWasSent) {
  Commit(2);
  StartSender(1);
  ASSERT_EQ(reader->Frames(2).size(), 2u);
  reader->Send(RespCommand({"REPLCONF", "ACK", "3"}));
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 1s));
}

TEST_F(SenderTest, DropsTheLinkWhenInputGrowsWithoutAFrame) {
  StartSender(1);
  // A bulk header promising far more than the input bound; the body never completes.
  reader->Send("*1\r\n$100000000\r\n");
  const std::string junk(size_t{64} * 1024, 'j');
  for (int i = 0; i < 4 && sender->alive(); ++i) {
    if (!WriteAll(reader->fd(), junk, 1s).ok()) break;
  }
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 1s));
}

TEST_F(SenderTest, DropsTheLinkWhenAcksStopForReplTimeout) {
  ReplSender::Callbacks cb;
  cb.repl_timeout = [] { return std::chrono::milliseconds(300); };
  StartSender(1, std::move(cb));
  EXPECT_TRUE(sender->alive());
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 2s));
}

TEST_F(SenderTest, AcksKeepTheLinkAlivePastReplTimeout) {
  ReplSender::Callbacks cb;
  cb.repl_timeout = [] { return std::chrono::milliseconds(300); };
  StartSender(1, std::move(cb));
  for (int i = 0; i < 8; ++i) {
    reader->Send(RespCommand({"REPLCONF", "ACK", "0"}));
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_TRUE(sender->alive());
}

TEST_F(SenderTest, StopIsPromptWhileWaiting) {
  StartSender(1);
  std::this_thread::sleep_for(100ms);  // inside the watermark wait
  const auto start = std::chrono::steady_clock::now();
  sender->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
  EXPECT_FALSE(sender->alive());
  sender->Stop();  // idempotent
}

TEST_F(SenderTest, StopIsPromptWhileBlockedOnAFullSocket) {
  for (int i = 0; i < 64; ++i) {
    ASSERT_TRUE(log.CommitStandalone({{ReplOpType::kPut, ColumnFamily::kMeta, "k" + std::to_string(i),
                                       std::string(size_t{64} * 1024, 'v')}})
                    .ok());
  }
  StartSender(1);  // the reader never reads: the sender blocks in WriteAll
  std::this_thread::sleep_for(100ms);
  const auto start = std::chrono::steady_clock::now();
  sender->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
}

TEST_F(SenderTest, DropsTheLinkWhenTheNextSeqWasTrimmed) {
  Commit(3);
  {
    auto txn = storage.BeginWrite();
    ASSERT_TRUE((*txn)->Delete(ColumnFamily::kReplog, encoding::ReplogKey(1)).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  StartSender(1);
  EXPECT_TRUE(WaitFor([&] { return !sender->alive(); }, 1s));
  EXPECT_TRUE(reader->For(50ms).empty());  // never skips a gap
}

// A replog entry committed to storage but not yet published by ReplLog (as when a commit is in flight
// under ReplLog::mu_) must not be sent until the watermark covers it.
TEST_F(SenderTest, NeverReadsBeyondTheWatermark) {
  Commit(3);
  {
    auto txn = storage.BeginWrite();
    ASSERT_TRUE((*txn)
                    ->Put(ColumnFamily::kReplog, encoding::ReplogKey(4),
                          encoding::EncodeReplogBatch(MetaPut("x"), 1, 0))
                    .ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ASSERT_EQ(log.last(), 3u);
  StartSender(1);
  const auto early = reader->For(300ms);
  ASSERT_EQ(early.size(), 3u);
  EXPECT_EQ(early[2][1], "3");
  ASSERT_TRUE(log.Recover().ok());  // publishes seq 4
  const auto frames = reader->Frames(4, 2s);
  ASSERT_EQ(frames.size(), 4u);
  EXPECT_EQ(frames[3], Batch(4, ReplogAt(4)));
}

TEST_F(SenderTest, ConcurrentCommitsArriveStrictlyConsecutive) {
  StartSender(1);
  constexpr int kCommits = 400;
  std::thread writer([&] {
    for (int i = 0; i < kCommits; ++i)
      ASSERT_TRUE(log.CommitStandalone(MetaPut("c" + std::to_string(i))).ok());
  });
  writer.join();
  std::vector<std::vector<std::string>> frames = reader->Frames(kCommits, 5s);
  std::vector<uint64_t> seqs;
  for (const auto& f : frames) {
    if (f[0] == "BATCH") seqs.push_back(std::stoull(f[1]));
  }
  ASSERT_EQ(seqs.size(), static_cast<size_t>(kCommits));
  for (size_t i = 0; i < seqs.size(); ++i) ASSERT_EQ(seqs[i], i + 1);
}

TEST_F(SenderTest, StreamsTheSnapshotThenTheLiveTail) {
  Commit(5);
  auto snap = log.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  ASSERT_EQ(snap->seq, 5u);
  Commit(2);  // after the cut: only BATCH frames carry these
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  reader = std::make_unique<Reader>(Fd(fds[1]));
  sender = std::make_unique<ReplSender>(storage, log, Fd(fds[0]), 6);
  sender->SetSnapshot(std::move(snap).value());
  sender->Start();
  const auto frames = reader->Frames(4);  // LOAD, LOADEND, BATCH 6, BATCH 7
  ASSERT_EQ(frames.size(), 4u);
  ASSERT_EQ(frames[0][0], "LOAD");
  auto ops = DecodeLoadChunk(frames[0][1]);
  ASSERT_TRUE(ops.ok());
  EXPECT_EQ(ops->size(), 5u);  // k1..k5, nothing written after the cut
  EXPECT_EQ(frames[1], (std::vector<std::string>{"LOADEND", "5", ReplogAt(5), "5"}));
  EXPECT_EQ(frames[2], Batch(6, ReplogAt(6)));
  EXPECT_EQ(frames[3], Batch(7, ReplogAt(7)));
  EXPECT_FALSE(sender->online());          // until the replica says it loaded
  EXPECT_EQ(sender->oldest_needed(), 6u);  // pins the trim at the cut (D2)
  reader->Send("*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n$1\r\n5\r\n");
  EXPECT_TRUE(WaitFor([&] { return sender->online(); }));
}

// PF9: at S = 0 the replica's first ACK is 0, which advances nothing but still means it loaded.
TEST_F(SenderTest, SnapshotOfAnEmptyStoreIsAnEmptyLoadEnd) {
  auto snap = log.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  reader = std::make_unique<Reader>(Fd(fds[1]));
  std::atomic<int> acks{0};
  ReplSender::Callbacks cb;
  cb.on_ack = [&acks] { ++acks; };
  sender = std::make_unique<ReplSender>(storage, log, Fd(fds[0]), 1, std::move(cb));
  sender->SetSnapshot(std::move(snap).value());
  sender->Start();
  const auto frames = reader->Frames(1);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0], (std::vector<std::string>{"LOADEND", "0", "", "0"}));
  EXPECT_FALSE(sender->online());
  reader->Send("*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n$1\r\n0\r\n");
  EXPECT_TRUE(WaitFor([&] { return sender->online(); }));
  EXPECT_EQ(acks.load(), 1);
  EXPECT_EQ(sender->acked(), 0u);
}

TEST_F(SenderTest, ASenderWithoutASnapshotIsOnlineAtStart) {
  Commit(1);
  StartSender(2);
  EXPECT_TRUE(sender->online());
}

TEST_F(SenderTest, SlowKeysAreKeptAliveWithPingAndStopIsPrompt) {
  Commit(5);
  auto snap = log.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  reader = std::make_unique<Reader>(Fd(fds[1]));
  ReplSender::Callbacks cb;
  cb.key_save_delay_us = [] { return 400000; };  // 0.4 s per key: the first chunk takes 2 s to build
  sender = std::make_unique<ReplSender>(storage, log, Fd(fds[0]), 6, cb);
  sender->SetSnapshot(std::move(snap).value());
  sender->Start();
  const auto frames = reader->For(1600ms);
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames[0], (std::vector<std::string>{"PING"}));
  const auto t0 = std::chrono::steady_clock::now();
  sender->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);  // checked between keys
}

}  // namespace
}  // namespace tideskv::repl
