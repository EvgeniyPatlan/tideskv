#include "server/connection_block_handler.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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
                                                deadline, request.timeout_reply);
  session_.blocked = client;
  std::string refusal;
  bool close_after = false;
  if (!registry_.AddIfCurrent(client, epoch_, &refusal, &close_after)) {
    // delivered like any unblock, after this command; a demotion also closes the connection (M6a D14), a
    // topology redirect (M7a D16) does not
    if (close_after) {
      client->UnblockAndClose(std::move(refusal));
    } else {
      client->Unblock(std::move(refusal));
    }
    return;
  }
  for (const std::string& k : client->keys()) ready_.push_back({client->db(), k});
}

}  // namespace tideskv
