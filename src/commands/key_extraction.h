#pragma once
#include <cstddef>
#include <string_view>
#include <vector>

#include "commands/registry.h"

// Which arguments of a call are keys (M7a D17): Redis getKeysFromCommand. The routing check (D13), COMMAND
// GETKEYS and COMMAND INFO's movablekeys flag use it.
namespace tideskv {

// The argv indexes of the keys, in argument order (duplicates kept): the command's movable-keys extractor
// when it has one, else its key-spec triple. A bad numkeys gives no keys (genericGetKeys): the command then
// runs and answers its own error.
std::vector<size_t> KeyIndexes(const CommandSpec& spec, const ArgList& argv);
bool HasMovableKeys(std::string_view name);   // Redis CMD_MOVABLE_KEYS: the extractor table
bool NoMandatoryKeys(std::string_view name);  // Redis CMD_NO_MANDATORY_KEYS: the EVAL family

}  // namespace tideskv
