#include "resp/request_parser.h"

#include <algorithm>
#include <cstdint>

#include "common/string_util.h"

namespace tideskv::resp {
namespace {

constexpr size_t kNpos = std::string_view::npos;

char Unescape(char e) {
  switch (e) {
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    default:
      return e;
  }
}

// Reads a quoted argument starting at line[*i] (the quote). Returns false if unbalanced.
bool ReadQuoted(std::string_view line, size_t* i, std::string* arg) {
  const char quote = line[(*i)++];
  while (*i < line.size()) {
    const char c = line[(*i)++];
    if (c == '\\' && *i < line.size()) {
      const char e = line[(*i)++];
      if (quote == '"') {
        arg->push_back(Unescape(e));
      } else if (e == '\'') {
        arg->push_back('\'');
      } else {
        arg->push_back('\\');
        arg->push_back(e);
      }
    } else if (c == quote) {
      // Redis requires whitespace (or end) after a closing quote.
      return *i == line.size() || line[*i] == ' ' || line[*i] == '\t';
    } else {
      arg->push_back(c);
    }
  }
  return false;
}

// Splits an inline command line on spaces/tabs, honouring "double" and 'single' quotes.
bool SplitInline(std::string_view line, std::vector<std::string>* out) {
  size_t i = 0;
  for (;;) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) return true;
    std::string arg;
    if (line[i] == '"' || line[i] == '\'') {
      if (!ReadQuoted(line, &i, &arg)) return false;
    } else {
      while (i < line.size() && line[i] != ' ' && line[i] != '\t') arg.push_back(line[i++]);
    }
    out->push_back(std::move(arg));
  }
}

}  // namespace

ParseStatus RequestParser::Parse(std::string_view data, size_t* consumed) {
  *consumed = 0;
  if (state_ == State::kError) return ParseStatus::kError;
  if (state_ == State::kIdle) {
    args_.clear();
    buffered_ = 0;
  }
  size_t pos = 0;
  for (;;) {
    Progress p = Progress::kError;
    switch (state_) {
      case State::kIdle:
        p = StartCommand(data, pos);
        break;
      case State::kArrayHeader:
        p = ParseArrayHeader(data, &pos);
        break;
      case State::kBulkHeader:
        p = ParseBulkHeader(data, &pos);
        break;
      case State::kBulkBody:
        p = ParseBulkBody(data, &pos);
        break;
      case State::kInline:
        p = ParseInline(data, &pos);
        break;
      case State::kError:
        p = Progress::kError;
        break;
    }
    if (p == Progress::kContinue) continue;
    *consumed = pos;
    if (p == Progress::kNeedMore) return ParseStatus::kNeedMore;
    return p == Progress::kCommand ? ParseStatus::kCommand : ParseStatus::kError;
  }
}

RequestParser::Progress RequestParser::StartCommand(std::string_view d, size_t pos) {
  if (pos >= d.size()) return Progress::kNeedMore;
  state_ = d[pos] == '*' ? State::kArrayHeader : State::kInline;
  return Progress::kContinue;
}

RequestParser::Progress RequestParser::ParseArrayHeader(std::string_view d, size_t* pos) {
  const size_t eol = d.find("\r\n", *pos);
  if (eol == kNpos) {
    return LineTooLong(d.size() - *pos) ? Fail("Protocol error: too big mbulk count string")
                                        : Progress::kNeedMore;
  }
  int64_t n = 0;
  if (!ParseInt64(d.substr(*pos + 1, eol - *pos - 1), &n) ||
      n > static_cast<int64_t>(limits_.max_multibulk_len)) {
    return Fail("Protocol error: invalid multibulk length");
  }
  *pos = eol + 2;
  if (n <= 0) {  // "*0" and "*-1" are ignored, like Redis
    state_ = State::kIdle;
    return Progress::kContinue;
  }
  remaining_ = static_cast<size_t>(n);
  args_.reserve(std::min<size_t>(remaining_, 1024));
  state_ = State::kBulkHeader;
  return Progress::kContinue;
}

RequestParser::Progress RequestParser::ParseBulkHeader(std::string_view d, size_t* pos) {
  if (*pos >= d.size()) return Progress::kNeedMore;
  if (d[*pos] != '$') return Fail(std::string("Protocol error: expected '$', got '") + d[*pos] + "'");
  const size_t eol = d.find("\r\n", *pos);
  if (eol == kNpos) {
    return LineTooLong(d.size() - *pos) ? Fail("Protocol error: too big bulk count string")
                                        : Progress::kNeedMore;
  }
  int64_t len = 0;
  if (!ParseInt64(d.substr(*pos + 1, eol - *pos - 1), &len) || len < 0 ||
      static_cast<uint64_t>(len) > limits_.max_bulk_len) {
    return Fail("Protocol error: invalid bulk length");
  }
  *pos = eol + 2;
  bulk_len_ = static_cast<size_t>(len);
  args_.emplace_back();
  args_.back().reserve(std::min<size_t>(bulk_len_, 64 * 1024));  // grow with data, never trust the header
  state_ = State::kBulkBody;
  return Progress::kContinue;
}

RequestParser::Progress RequestParser::ParseBulkBody(std::string_view d, size_t* pos) {
  std::string& arg = args_.back();
  const size_t take = std::min(bulk_len_ - arg.size(), d.size() - *pos);
  arg.append(d.substr(*pos, take));
  *pos += take;
  buffered_ += take;
  if (arg.size() < bulk_len_ || d.size() - *pos < 2) return Progress::kNeedMore;
  if (d[*pos] != '\r' || d[*pos + 1] != '\n') return Fail("Protocol error: expected CRLF after bulk string");
  *pos += 2;
  if (--remaining_ > 0) {
    state_ = State::kBulkHeader;
    return Progress::kContinue;
  }
  state_ = State::kIdle;
  return Progress::kCommand;
}

RequestParser::Progress RequestParser::ParseInline(std::string_view d, size_t* pos) {
  const size_t nl = d.find('\n', *pos);
  if (nl == kNpos) {
    return LineTooLong(d.size() - *pos) ? Fail("Protocol error: too big inline request")
                                        : Progress::kNeedMore;
  }
  std::string_view line = d.substr(*pos, nl - *pos);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  if (LineTooLong(line.size())) return Fail("Protocol error: too big inline request");
  *pos = nl + 1;
  if (!SplitInline(line, &args_)) return Fail("Protocol error: unbalanced quotes in request");
  state_ = State::kIdle;
  return args_.empty() ? Progress::kContinue : Progress::kCommand;  // blank line: ignore
}

RequestParser::Progress RequestParser::Fail(std::string message) {
  state_ = State::kError;
  error_ = std::move(message);
  return Progress::kError;
}

}  // namespace tideskv::resp
