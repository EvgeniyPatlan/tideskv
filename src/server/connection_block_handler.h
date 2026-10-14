#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "commands/registry.h"
#include "server/blocking.h"

namespace tideskv {

// The BlockHandler of a real connection (and of CommandHarness): parks the session's command in the
// registry. Its own keys go into `ready`, so the serve loop right after the command re-checks them. That
// closes the race with a push that committed between the handler's empty check and the registration.
class ConnectionBlockHandler final : public BlockHandler {
 public:
  ConnectionBlockHandler(BlockingRegistry& registry, Session& session, const std::vector<std::string>& argv,
                         ReadyKeys& ready)
      : registry_(registry), session_(session), argv_(argv), ready_(ready), epoch_(registry.epoch()) {}

  void Block(const BlockRequest& request) override;

 private:
  BlockingRegistry& registry_;
  Session& session_;
  const std::vector<std::string>& argv_;
  ReadyKeys& ready_;
  // Read when the command is dispatched, before it checks the role: a demotion's DisconnectAll after
  // this point makes Block refuse (UNBLOCKED, then close) instead of waiting on a replica forever.
  const uint64_t epoch_;
};

}  // namespace tideskv
