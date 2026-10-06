#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tideskv::resp {

struct ParserLimits {
  size_t max_bulk_len = 512ULL * 1024 * 1024;  // proto-max-bulk-len
  size_t max_multibulk_len = 1024 * 1024;      // max arguments in one command
  size_t max_inline_len = 64 * 1024;           // inline command or header line
};

enum class ParseStatus { kNeedMore, kCommand, kError };

// Incremental parser for client requests: RESP arrays of bulk strings, and inline
// commands ("PING\r\n"). Partial state survives between calls, so the caller must
// erase exactly *consumed bytes and pass the remainder (plus new data) next time.
class RequestParser {
 public:
  explicit RequestParser(ParserLimits limits = {}) : limits_(limits) {}

  ParseStatus Parse(std::string_view data, size_t* consumed);

  const std::vector<std::string>& args() const { return args_; }  // valid after kCommand
  std::vector<std::string>& mutable_args() { return args_; }
  const std::string& error() const { return error_; }  // valid after kError (sticky)
  size_t buffered_bytes() const { return buffered_; }  // partial-argument bytes held

 private:
  enum class State { kIdle, kArrayHeader, kBulkHeader, kBulkBody, kInline, kError };
  enum class Progress { kContinue, kNeedMore, kCommand, kError };

  Progress StartCommand(std::string_view d, size_t pos);
  Progress ParseArrayHeader(std::string_view d, size_t* pos);
  Progress ParseBulkHeader(std::string_view d, size_t* pos);
  Progress ParseBulkBody(std::string_view d, size_t* pos);
  Progress ParseInline(std::string_view d, size_t* pos);
  Progress Fail(std::string message);
  bool LineTooLong(size_t pending) const { return pending > limits_.max_inline_len; }

  ParserLimits limits_;
  State state_ = State::kIdle;
  std::vector<std::string> args_;
  std::string error_;
  size_t remaining_ = 0;  // bulk strings still expected in this array
  size_t bulk_len_ = 0;   // length of the bulk string being read
  size_t buffered_ = 0;
};

}  // namespace tideskv::resp
