#pragma once
#include <string>
#include <string_view>

#include "commands/registry.h"
#include "common/status.h"
#include "types/errors.h"  // re-exported: handlers use the kErr* constants

// Status -> reply mapping and message helpers for the data commands.
namespace tideskv {

std::string InvalidExpireError(std::string_view command);  // "ERR invalid expire time in 'set' command"
std::string ArityError(std::string_view command);          // "ERR wrong number of arguments for 'x' command"
std::string AtLeastOneKeyError(
    std::string_view command);  // "ERR at least 1 input key is needed for 'x' command"
std::string UnsupportedOptionError(std::string_view opt);  // "ERR Unsupported option KEEPTTL"
// "ERR unknown subcommand 'sub'. Try SCRIPT HELP.", sub cut to 128 bytes (Redis: '%.128s').
std::string UnknownSubcommandError(std::string_view sub, std::string_view command);
// addReplySubcommandSyntaxError: "ERR unknown subcommand or wrong number of arguments for 'sub'. Try DEBUG
// HELP.", sub cut to 128 bytes.
std::string SubcommandSyntaxError(std::string_view sub, std::string_view command);

// Error reply for a failed status: kUserError verbatim; storage failures per spec §5.4,
// with details logged server-side and never sent to the client.
void ReplyError(ExecContext& ctx, const Status& st);

}  // namespace tideskv
