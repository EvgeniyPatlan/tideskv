#include "resp/reply_parser.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace tideskv::resp {
namespace {

constexpr int kMaxDepth = 10000;  // deeper than any reply our handlers write

class Parser {
 public:
  explicit Parser(std::string_view d) : d_(d) {}
  Result<ReplyNode> One(int depth);
  bool AtEnd() const { return pos_ == d_.size(); }

 private:
  bool Line(std::string_view* out) {
    const size_t e = d_.find("\r\n", pos_);
    if (e == std::string_view::npos) return false;
    *out = d_.substr(pos_, e - pos_);
    pos_ = e + 2;
    return true;
  }
  static bool Int(std::string_view s, int64_t* v) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), *v);
    return ec == std::errc() && p == s.data() + s.size();
  }
  Result<ReplyNode> Bulk(ReplyNode::Type type, std::string_view header);
  Result<ReplyNode> Aggregate(ReplyNode::Type type, std::string_view header, int depth);
  static Result<ReplyNode> Scalar(char prefix, std::string_view line);
  static Status Bad() { return Status::Internal("malformed reply"); }

  std::string_view d_;
  size_t pos_ = 0;
};

Result<ReplyNode> Parser::Bulk(ReplyNode::Type type, std::string_view header) {
  int64_t n = 0;
  if (!Int(header, &n)) return Bad();
  ReplyNode node;
  if (n < 0) {
    node.type = ReplyNode::Type::kNullBulk;
    return node;
  }
  if (pos_ + static_cast<size_t>(n) + 2 > d_.size()) return Bad();
  std::string_view body = d_.substr(pos_, static_cast<size_t>(n));
  pos_ += static_cast<size_t>(n) + 2;
  node.type = type;
  if (type == ReplyNode::Type::kVerbatim) {
    if (body.size() < 4 || body[3] != ':') return Bad();
    node.format = std::string(body.substr(0, 3));
    body.remove_prefix(4);
  }
  node.str = std::string(body);
  return node;
}

Result<ReplyNode> Parser::Aggregate(ReplyNode::Type type, std::string_view header, int depth) {
  int64_t n = 0;
  if (!Int(header, &n)) return Bad();
  ReplyNode node;
  if (n < 0) {
    node.type = ReplyNode::Type::kNullArray;
    return node;
  }
  node.type = type;
  const int64_t count = type == ReplyNode::Type::kMap ? 2 * n : n;
  for (int64_t i = 0; i < count; ++i) {
    auto e = One(depth + 1);
    if (!e.ok()) return e.status();
    node.elems.push_back(std::move(e).value());
  }
  return node;
}

Result<ReplyNode> Parser::Scalar(char prefix, std::string_view line) {
  ReplyNode node;
  switch (prefix) {
    case '+':
      node.type = ReplyNode::Type::kStatus;
      node.str = std::string(line);
      return node;
    case '-':
      node.type = ReplyNode::Type::kError;
      node.str = std::string(line);
      return node;
    case ':':
      node.type = ReplyNode::Type::kInteger;
      return Int(line, &node.integer) ? Result<ReplyNode>(node) : Bad();
    case '_':
      node.type = ReplyNode::Type::kNull;
      return node;
    case '#':
      node.type = ReplyNode::Type::kBool;
      node.boolean = line == "t";
      return node;
    case '(':
      node.type = ReplyNode::Type::kBigNumber;
      node.str = std::string(line);
      return node;
    case ',':
      node.type = ReplyNode::Type::kDouble;
      if (line == "inf")
        node.dbl = std::numeric_limits<double>::infinity();
      else if (line == "-inf")
        node.dbl = -std::numeric_limits<double>::infinity();
      else if (line == "nan")
        node.dbl = std::nan("");
      else
        node.dbl = std::strtod(std::string(line).c_str(), nullptr);
      return node;
    default:
      return Bad();
  }
}

Result<ReplyNode> Parser::One(int depth) {
  if (depth > kMaxDepth || pos_ >= d_.size()) return Bad();
  const char prefix = d_[pos_++];
  std::string_view line;
  if (!Line(&line)) return Bad();
  switch (prefix) {
    case '$':
      return Bulk(ReplyNode::Type::kBulk, line);
    case '=':
      return Bulk(ReplyNode::Type::kVerbatim, line);
    case '*':
      return Aggregate(ReplyNode::Type::kArray, line, depth);
    case '%':
      return Aggregate(ReplyNode::Type::kMap, line, depth);
    case '~':
      return Aggregate(ReplyNode::Type::kSet, line, depth);
    case '|': {  // attribute: skip its map, return the reply that follows (script_lua.c:414)
      auto attr = Aggregate(ReplyNode::Type::kMap, line, depth);
      if (!attr.ok()) return attr.status();
      return One(depth);
    }
    default:
      return Scalar(prefix, line);
  }
}

}  // namespace

Result<ReplyNode> ParseReply(std::string_view data) {
  Parser p(data);
  auto node = p.One(0);
  if (!node.ok()) return node;
  if (!p.AtEnd()) return Status::Internal("trailing bytes after reply");
  return node;
}

}  // namespace tideskv::resp
