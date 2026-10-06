#pragma once
#include <string>

#include "commands/all_commands.h"
#include "support/fake_server_services.h"

namespace tideskv::test {

// Runs commands through the real registry + Dispatch, returning raw reply bytes.
struct CommandHarness {
  CommandHarness() {
    RegisterAllCommands(services.registry_);
    session.id = 7;
    session.authenticated = true;
  }

  std::string Run(const ArgList& argv) {
    ExecContext ctx{session, reply, services};
    Dispatch(services.registry(), ctx, argv);
    return reply.Take();
  }

  void RequirePassword(const std::string& pass) {
    EXPECT_TRUE(services.config_.Set("requirepass", pass, true).ok());
    session.authenticated = false;
  }

  FakeServerServices services;
  Session session;
  resp::ReplyBuilder reply;
};

}  // namespace tideskv::test
