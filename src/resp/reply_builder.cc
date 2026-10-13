#include "resp/reply_builder.h"

#include <utility>

#include "common/score_format.h"

namespace tideskv::resp {

void ReplyBuilder::Line(char prefix, std::string_view s) {
  buf_.push_back(prefix);
  buf_.append(s);
  buf_.append("\r\n");
}

void ReplyBuilder::SanitizedLine(char prefix, std::string_view s) {
  buf_.push_back(prefix);
  for (char c : s) buf_.push_back((c == '\r' || c == '\n') ? ' ' : c);
  buf_.append("\r\n");
}

void ReplyBuilder::SimpleString(std::string_view s) { SanitizedLine('+', s); }
void ReplyBuilder::Error(std::string_view msg) { SanitizedLine('-', msg); }
void ReplyBuilder::Integer(int64_t v) { Line(':', std::to_string(v)); }

void ReplyBuilder::Bulk(std::string_view s) {
  Count('$', s.size());
  buf_.append(s);
  buf_.append("\r\n");
}

void ReplyBuilder::Null() { buf_.append(protocol_ >= 3 ? "_\r\n" : "$-1\r\n"); }
void ReplyBuilder::NullArray() { buf_.append(protocol_ >= 3 ? "_\r\n" : "*-1\r\n"); }
void ReplyBuilder::ArrayHeader(size_t n) { Count('*', n); }
void ReplyBuilder::MapHeader(size_t n) { protocol_ >= 3 ? Count('%', n) : Count('*', 2 * n); }
void ReplyBuilder::SetHeader(size_t n) { Count(protocol_ >= 3 ? '~' : '*', n); }

void ReplyBuilder::Double(double d) {
  if (protocol_ >= 3) {
    Line(',', FormatScore(d));
  } else {
    Bulk(FormatScore(d));
  }
}

void ReplyBuilder::Boolean(bool b) {
  if (protocol_ >= 3) {
    buf_.append(b ? "#t\r\n" : "#f\r\n");
  } else {
    Integer(b ? 1 : 0);
  }
}

void ReplyBuilder::Verbatim(std::string_view text) {
  if (protocol_ < 3) {
    Bulk(text);
    return;
  }
  Count('=', text.size() + 4);
  buf_.append("txt:");
  buf_.append(text);
  buf_.append("\r\n");
}

std::string ReplyBuilder::Take() { return std::exchange(buf_, std::string()); }

}  // namespace tideskv::resp
