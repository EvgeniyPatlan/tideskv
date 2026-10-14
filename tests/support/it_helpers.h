#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "support/resp_client.h"

// Helpers shared by the integration tests (header-only).
namespace tideskv::test {

// The integer value of `field` in INFO `section`, or -1 when the field is missing.
inline int64_t InfoField(RespClient& c, const std::string& section, const std::string& field) {
  const std::string info = c.Command({"INFO", section}).str;
  const size_t at = info.find(field + ":");
  return at == std::string::npos ? -1 : std::stoll(info.substr(at + field.size() + 1));
}

// Like InfoField, for a non-numeric value ("" when the field is absent).
inline std::string InfoString(RespClient& c, const std::string& section, const std::string& field) {
  const std::string info = c.Command({"INFO", section}).str;
  const size_t at = info.find(field + ":");
  if (at == std::string::npos) return "";
  const size_t from = at + field.size() + 1;
  return info.substr(from, info.find("\r\n", from) - from);
}

// Polls cond every 5 ms until it holds or `timeout` passes; its last value.
inline bool WaitFor(const std::function<bool()>& cond,
                    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cond();
}

}  // namespace tideskv::test
