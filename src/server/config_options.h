#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "server/config.h"

namespace tideskv {

// One CONFIG parameter: its name, how to set it from text and how to render it.
struct Option {
  std::string_view name;
  bool mutable_at_runtime;
  Status (*set)(ConfigValues&, std::string_view);
  std::string (*get)(const ConfigValues&);
  bool alias = false;      // legacy name of another option: readable/settable, never rewritten
  bool multi_arg = false;  // value is several words, written to the file unquoted ("replicaof h p");
                           // an empty value is not written at all
};

// Replication parameters (Redis 7.2 names, aliases, ranges and error texts).
const std::vector<Option>& ReplOptions();

}  // namespace tideskv
