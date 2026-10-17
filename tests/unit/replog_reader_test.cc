#include "repl/replog_reader.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "encoding/replog_codec.h"
#include "support/command_harness.h"

namespace tideskv::repl {
namespace {

using test::CommandHarness;
using test::kOk;

std::vector<uint64_t> ReadSeqs(ReplogReader& r, uint64_t last, size_t max_bytes, Status* st) {
  std::vector<uint64_t> seqs;
  *st = r.Read(last, max_bytes, [&](uint64_t seq, std::string_view payload) {
    EXPECT_TRUE(encoding::DecodeReplogBatch(payload).ok());
    seqs.push_back(seq);
    return Status::Ok();
  });
  return seqs;
}

TEST(ReplogReader, ReadsInSeqOrderUpToTheWatermarkAndResumes) {
  CommandHarness h;
  for (int i = 0; i < 5; ++i) ASSERT_EQ(h.Run({"SET", "k" + std::to_string(i), "v"}), kOk);
  const uint64_t last = h.services.repl_log_.last();
  ReplogReader r(h.services.storage_, 1);
  Status st;
  EXPECT_EQ(ReadSeqs(r, 3, 1 << 20, &st), (std::vector<uint64_t>{1, 2, 3}));
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(r.next(), 4u);
  EXPECT_EQ(ReadSeqs(r, last, 1, &st), (std::vector<uint64_t>{4}));  // at least one batch per read
  EXPECT_EQ(ReadSeqs(r, last, 1 << 20, &st), (std::vector<uint64_t>{5}));
  EXPECT_TRUE(ReadSeqs(r, last, 1 << 20, &st).empty());  // caught up: OK, nothing
  EXPECT_TRUE(st.ok());
}

TEST(ReplogReader, ATrimmedSeqIsNotRetained) {  // D19 / R16
  CommandHarness h;
  for (int i = 0; i < 4; ++i) ASSERT_EQ(h.Run({"SET", "k" + std::to_string(i), "v"}), kOk);
  ASSERT_TRUE(h.services.repl_log_.Trim(2).ok());
  ReplogReader r(h.services.storage_, 2);
  Status st;
  EXPECT_TRUE(ReadSeqs(r, h.services.repl_log_.last(), 1 << 20, &st).empty());
  EXPECT_EQ(st.code(), StatusCode::kNotFound);
  EXPECT_EQ(st.message(), "replog entry 2 is not retained");
}

TEST(ReplogReader, AnErrorFromTheCallbackStopsBeforeThatBatch) {
  CommandHarness h;
  for (int i = 0; i < 3; ++i) ASSERT_EQ(h.Run({"SET", "k" + std::to_string(i), "v"}), kOk);
  ReplogReader r(h.services.storage_, 1);
  const Status st = r.Read(3, 1 << 20, [](uint64_t seq, std::string_view) {
    return seq == 2 ? Status::IOError("peer gone") : Status::Ok();
  });
  EXPECT_EQ(st.code(), StatusCode::kIOError);
  EXPECT_EQ(r.next(), 2u);
}

}  // namespace
}  // namespace tideskv::repl
