#include "commands/all_commands.h"

namespace tideskv {

void RegisterAllCommands(CommandRegistry& registry) {
  RegisterConnectionCommands(registry);
  RegisterServerCommands(registry);
  RegisterStringCommands(registry);
  RegisterStringEditCommands(registry);
  RegisterCounterCommands(registry);
  RegisterLcsCommand(registry);
  RegisterKeyspaceCommands(registry);
  RegisterExpireCommands(registry);
  RegisterScanCommands(registry);
  RegisterHashCommands(registry);
  RegisterSetCommands(registry);
  RegisterSetAlgebraCommands(registry);
  RegisterListCommands(registry);
  RegisterListEditCommands(registry);
  RegisterElementScanCommands(registry);
}

}  // namespace tideskv
