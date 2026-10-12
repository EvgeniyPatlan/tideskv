#pragma once
#include <string>
#include <vector>

namespace tideskv::test {

// Parses "*2\r\n$<n>\r\n<cursor>\r\n*<m>\r\n($<k>\r\n<key>\r\n)*" (binary-safe).
struct ScanReply {
  std::string cursor;
  std::vector<std::string> keys;
};

inline std::string TakeBulk(const std::string& s, size_t* pos) {
  const size_t crlf = s.find("\r\n", *pos);
  const size_t n = std::stoul(s.substr(*pos + 1, crlf - *pos - 1));
  std::string out = s.substr(crlf + 2, n);
  *pos = crlf + 2 + n + 2;
  return out;
}

inline ScanReply ParseScan(const std::string& raw) {
  ScanReply r;
  size_t pos = raw.find("\r\n") + 2;  // skip *2
  r.cursor = TakeBulk(raw, &pos);
  const size_t crlf = raw.find("\r\n", pos);
  const size_t m = std::stoul(raw.substr(pos + 1, crlf - pos - 1));
  pos = crlf + 2;
  for (size_t i = 0; i < m; ++i) r.keys.push_back(TakeBulk(raw, &pos));
  return r;
}

}  // namespace tideskv::test
