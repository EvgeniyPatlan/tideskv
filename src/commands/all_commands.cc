#include "commands/all_commands.h"

namespace tideskv {

void RegisterAllCommands(CommandRegistry& registry) {
  RegisterConnectionCommands(registry);
  RegisterServerCommands(registry);
  RegisterStringCommands(registry);
  RegisterStringEditCommands(registry);
  RegisterCounterCommands(registry);
}

}  // namespace tideskv
