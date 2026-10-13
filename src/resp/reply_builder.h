#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tideskv::resp {

// Appends RESP2 or RESP3 encoded replies to an output buffer. One per connection;
// HELLO switches the protocol before its own reply is written.
class ReplyBuilder {
 public:
  explicit ReplyBuilder(int protocol = 2) : protocol_(protocol) {}

  void SetProtocol(int protocol) { protocol_ = protocol; }
  int protocol() const { return protocol_; }

  void SimpleString(std::string_view s);
  void Error(std::string_view msg);  // full text incl. code word, e.g. "ERR syntax error"
  void Integer(int64_t v);
  void Bulk(std::string_view s);
  void Null();       // RESP2 "$-1", RESP3 "_"
  void NullArray();  // RESP2 "*-1", RESP3 "_"
  void ArrayHeader(size_t n);
  void MapHeader(size_t n);              // RESP2: flat array of 2n
  void SetHeader(size_t n);              // RESP2: array
  void Double(double d);                 // RESP2: bulk string
  void Boolean(bool b);                  // RESP2: integer 0/1
  void Verbatim(std::string_view text);  // RESP3 "=…txt:"; RESP2 bulk
  // RESP3 "=<len+4>\r\n<fmt3>:<text>", format padded/truncated to 3 chars; RESP2 bulk.
  void Verbatim(std::string_view text, std::string_view format);
  void BigNumber(std::string_view digits);  // RESP2: bulk; RESP3 "("
  // Already-encoded reply bytes (the reply of an unblocked command).
  void Raw(std::string_view bytes) { buf_.append(bytes); }

  const std::string& buffer() const { return buf_; }
  std::string Take();
  size_t size() const { return buf_.size(); }
  bool empty() const { return buf_.empty(); }
  void Truncate(size_t n) {  // drops bytes after n (no-op if the buffer is not larger)
    if (n < buf_.size()) buf_.resize(n);
  }

 private:
  void Line(char prefix, std::string_view s);           // prefix + s + CRLF
  void SanitizedLine(char prefix, std::string_view s);  // CR/LF replaced by spaces
  void Count(char prefix, size_t n) { Line(prefix, std::to_string(n)); }

  int protocol_;
  std::string buf_;
};

}  // namespace tideskv::resp
