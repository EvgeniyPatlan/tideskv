#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tideskv::test {

struct RespValue {
  enum class Type {
    kSimple,
    kError,
    kInteger,
    kBulk,
    kNull,
    kArray,
    kMap,
    kSet,
    kDouble,
    kBoolean,
    kVerbatim
  };
  Type type = Type::kNull;
  std::string str;                  // simple, error, bulk, verbatim (without "txt:"), double text
  int64_t integer = 0;              // integer; boolean as 0/1
  std::vector<RespValue> elements;  // array/set; map as k1, v1, k2, v2, ...
};

std::string ToDebugString(const RespValue& v);
std::string EncodeCommand(const std::vector<std::string>& argv);

// Minimal blocking RESP2/RESP3 client for tests. Not thread-safe.
class RespClient {
 public:
  static std::unique_ptr<RespClient> Connect(uint16_t port,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(5));
  ~RespClient();
  RespClient(const RespClient&) = delete;
  RespClient& operator=(const RespClient&) = delete;

  bool SendRaw(std::string_view bytes);
  bool Send(const std::vector<std::string>& argv) { return SendRaw(EncodeCommand(argv)); }
  std::optional<RespValue> Read();  // nullopt on EOF, error or timeout
  // Send + Read. Records a test failure and returns an error value if no reply arrives.
  RespValue Command(const std::vector<std::string>& argv);
  bool WaitForClose(std::chrono::milliseconds timeout = std::chrono::seconds(2));  // true if peer closed
  void Close();

 private:
  explicit RespClient(int fd) : fd_(fd) {}
  bool Fill();
  std::optional<std::string> ReadLine();
  std::optional<std::string> ReadExact(size_t n);
  std::optional<RespValue> ReadAggregate(RespValue::Type type, int64_t count);
  std::optional<RespValue> ReadValue();

  int fd_;
  std::string buf_;
  size_t pos_ = 0;
};

}  // namespace tideskv::test
