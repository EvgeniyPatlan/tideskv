#include "server/connection_block_handler.h"

#include <chrono>
#include <memory>
#include <optional>

namespace tideskv {

void ConnectionBlockHandler::Block(const BlockRequest& request) {
  using Clock = BlockedClient::Clock;
  std::optional<Clock::time_point> deadline;
  if (request.timeout_ms > 0) {
    const auto now = Clock::now();
    // A timeout steady_clock cannot represent (e.g. 1e10 s; ParseBlockTimeout allows up to INT64_MAX ms)
    // would overflow; it blocks forever instead, as Redis effectively does.
    const auto headroom =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    if (request.timeout_ms < headroom.count()) deadline = now + std::chrono::milliseconds(request.timeout_ms);
  }
  Session copy = session_;
  copy.blocked.reset();
  auto client = std::make_shared<BlockedClient>(registry_, std::move(copy), argv_, request.type, request.keys,
                                                deadline);
  registry_.Add(client);
  session_.blocked = client;
  for (const std::string& k : client->keys()) ready_.push_back({client->db(), k});
}

}  // namespace tideskv
