#pragma once
#include "commands/registry.h"
#include "commands/server_services.h"

namespace tideskv::test {

class FakeServerServices final : public ServerServices {
 public:
  Config& config() override { return config_; }
  const CommandRegistry& registry() const override { return registry_; }
  ServerStats Stats() const override { return stats_; }
  void ResetStats() override { ++reset_calls_; }
  void RequestShutdown() override { shutdown_requested_ = true; }

  CommandRegistry registry_;
  Config config_;
  ServerStats stats_;
  int reset_calls_ = 0;
  bool shutdown_requested_ = false;
};

}  // namespace tideskv::test
