#include "server/connection_block_handler.h"

#include <chrono>
#include <memory>
#include <optional>

namespace tideskv {

void ConnectionBlockHandler::Block(const BlockRequest& request) {
  std::optional<BlockedClient::Clock::time_point> deadline;
  if (request.timeout_ms > 0)
    deadline = BlockedClient::Clock::now() + std::chrono::milliseconds(request.timeout_ms);
  Session copy = session_;
  copy.blocked.reset();
  auto client = std::make_shared<BlockedClient>(registry_, std::move(copy), argv_, request.type, request.keys,
                                                deadline);
  registry_.Add(client);
  session_.blocked = client;
  for (const std::string& k : client->keys()) ready_.push_back({client->db(), k});
}

}  // namespace tideskv
