#pragma once
#include <string>
#include <vector>

#include "common/string_util.h"
#include "encoding/key_layout.h"

namespace tideskv::test {

// What a fake master answers to a replica's handshake command before PSYNC: +PONG to PING, its key layout to
// REPLCONF key-layout (the process's, unless `layout` names another), +OK to the rest.
inline std::string FakeMasterReply(const std::vector<std::string>& cmd, const char* layout = nullptr) {
  if (!cmd.empty() && EqualsIgnoreCase(cmd[0], "ping")) return "+PONG\r\n";
  if (cmd.size() >= 2 && EqualsIgnoreCase(cmd[0], "replconf") && EqualsIgnoreCase(cmd[1], "key-layout")) {
    return "+" +
           std::string(layout != nullptr ? layout : encoding::KeyLayoutName(encoding::CurrentKeyLayout())) +
           "\r\n";
  }
  return "+OK\r\n";
}

}  // namespace tideskv::test
