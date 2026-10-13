#pragma once
#include <optional>
#include <string_view>

#include "commands/registry.h"
#include "common/status.h"
#include "types/list_move.h"
#include "types/list_type.h"

// List reply helpers shared by the list command files and the blocking commands.
namespace tideskv {

// LEFT/RIGHT (any case) or kErrSyntax.
Status ParseListEndArg(std::string_view arg, ListEnd* out);
// [key, [values...]], or a null array for nullopt (LMPOP/BLMPOP).
void ReplyPopped(ExecContext& ctx, const std::optional<PoppedFrom>& popped);

}  // namespace tideskv
