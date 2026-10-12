#include <optional>
#include <string>

#include "commands/all_commands.h"
#include "commands/server_services.h"
#include "common/string_util.h"

namespace tideskv {
namespace {

constexpr std::string_view kWrongPass = "WRONGPASS invalid username-password pair or user is disabled.";
constexpr std::string_view kBadName =
    "ERR Client names cannot contain spaces, newlines or special characters.";

void ArityError(ExecContext& ctx, std::string_view name) {
  ctx.reply.Error("ERR wrong number of arguments for '" + std::string(name) + "' command");
}

// nullopt on success, otherwise the error reply. `legacy_form` = AUTH <password>.
std::optional<std::string> CheckCredentials(ServerServices& server, std::string_view user,
                                            std::string_view pass, bool legacy_form) {
  const std::string required = server.config().Snapshot().requirepass;
  if (required.empty()) {
    if (legacy_form) {
      return "ERR AUTH <password> called without any password configured for the default user. "
             "Are you sure your configuration is correct?";
    }
    return user == "default" ? std::nullopt : std::optional<std::string>(kWrongPass);
  }
  if (user != "default" || !ConstantTimeEquals(pass, required)) return std::string(kWrongPass);
  return std::nullopt;
}

bool ValidClientName(std::string_view name) {
  for (char c : name) {
    if (c < '!' || c > '~') return false;
  }
  return true;
}

void Ping(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() > 2) return ArityError(ctx, "ping");
  if (argv.size() == 2) {
    ctx.reply.Bulk(argv[1]);
  } else {
    ctx.reply.SimpleString("PONG");
  }
}

void Echo(ExecContext& ctx, const ArgList& argv) { ctx.reply.Bulk(argv[1]); }

void Quit(ExecContext& ctx, const ArgList&) {
  ctx.reply.SimpleString("OK");
  ctx.session.close_after_reply = true;
}

void Auth(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() > 3) return ctx.reply.Error("ERR syntax error");
  const bool legacy = argv.size() == 2;
  const std::string_view user = legacy ? std::string_view("default") : std::string_view(argv[1]);
  if (auto err = CheckCredentials(ctx.server, user, argv.back(), legacy)) return ctx.reply.Error(*err);
  ctx.session.authenticated = true;
  ctx.reply.SimpleString("OK");
}

void WriteHelloReply(ExecContext& ctx) {
  resp::ReplyBuilder& r = ctx.reply;
  r.MapHeader(7);
  r.Bulk("server");
  r.Bulk("redis");
  r.Bulk("version");
  r.Bulk(kRedisCompatVersion);
  r.Bulk("proto");
  r.Integer(ctx.session.protocol);
  r.Bulk("id");
  r.Integer(static_cast<int64_t>(ctx.session.id));
  r.Bulk("mode");
  r.Bulk("standalone");
  r.Bulk("role");
  r.Bulk("master");
  r.Bulk("modules");
  r.ArrayHeader(0);
}

// Result of parsing HELLO options.
struct HelloOptions {
  int proto = 2;
  std::optional<std::pair<std::string_view, std::string_view>> auth{};  // user, pass
  std::optional<std::string_view> setname{};
};

// Parses "HELLO [protover [AUTH user pass] [SETNAME name]]". Collects options without applying.
// Returns error reply or nullopt on success (check options field).
std::optional<std::string> ParseHello(const ArgList& argv, int current_proto, HelloOptions* out) {
  out->proto = current_proto;
  size_t i = 1;
  if (argv.size() > 1) {
    int64_t v = 0;
    if (!ParseInt64(argv[1], &v)) return "ERR Protocol version is not an integer or out of range";
    if (v != 2 && v != 3) return "NOPROTO unsupported protocol version";
    out->proto = static_cast<int>(v);
    i = 2;
  }
  for (; i < argv.size(); ++i) {
    if (EqualsIgnoreCase(argv[i], "AUTH")) {
      if (i + 2 >= argv.size()) return "ERR Syntax error in HELLO option 'AUTH'";
      out->auth.emplace(argv[i + 1], argv[i + 2]);
      i += 2;
    } else if (EqualsIgnoreCase(argv[i], "SETNAME")) {
      if (i + 1 >= argv.size()) return "ERR Syntax error in HELLO option 'SETNAME'";
      out->setname.emplace(argv[i + 1]);
      ++i;
    } else {
      return "ERR Syntax error in HELLO option '" + argv[i] + "'";
    }
  }
  return std::nullopt;
}

void Hello(ExecContext& ctx, const ArgList& argv) {
  HelloOptions opts{};
  // Step a: Parse everything first, return error if syntax invalid (no session mutation)
  if (auto err = ParseHello(argv, ctx.session.protocol, &opts)) return ctx.reply.Error(*err);

  // Step b: If AUTH pair was given, validate credentials once (failure = error, session unchanged)
  if (opts.auth) {
    if (auto err = CheckCredentials(ctx.server, opts.auth->first, opts.auth->second, false)) {
      return ctx.reply.Error(*err);
    }
    ctx.session.authenticated = true;
  }

  // Step c: Check if authenticated (after optional auth)
  if (!ctx.session.authenticated) {
    return ctx.reply.Error(
        "NOAUTH HELLO must be called with the client already authenticated, otherwise the HELLO <proto> AUTH "
        "<user> <pass> option can be used to authenticate the client and select the RESP protocol version at "
        "the same time");
  }

  // Step d: Validate and apply SETNAME (name validation error doesn't undo authentication)
  if (opts.setname) {
    if (!ValidClientName(*opts.setname)) return ctx.reply.Error(kBadName);
    ctx.session.name = *opts.setname;
  }

  // Step e: Switch protocol and reply
  ctx.session.protocol = opts.proto;
  ctx.reply.SetProtocol(opts.proto);  // HELLO's own reply already uses the new protocol
  WriteHelloReply(ctx);
}

void ClientSetInfo(ExecContext& ctx, const ArgList& argv) {
  const std::string attr = ToLower(argv[2]);
  if (attr != "lib-name" && attr != "lib-ver")
    return ctx.reply.Error("ERR Unrecognized option '" + argv[2] + "'");
  if (!ValidClientName(argv[3])) return ctx.reply.Error(kBadName);
  (attr == "lib-name" ? ctx.session.lib_name : ctx.session.lib_ver) = argv[3];
  ctx.reply.SimpleString("OK");
}

void Client(ExecContext& ctx, const ArgList& argv) {
  const std::string sub = ToLower(argv[1]);
  if (sub == "id" && argv.size() == 2) return ctx.reply.Integer(static_cast<int64_t>(ctx.session.id));
  if (sub == "getname" && argv.size() == 2) {
    return ctx.session.name.empty() ? ctx.reply.Null() : ctx.reply.Bulk(ctx.session.name);
  }
  if (sub == "setname" && argv.size() == 3) {
    if (!ValidClientName(argv[2])) return ctx.reply.Error(kBadName);
    ctx.session.name = argv[2];
    return ctx.reply.SimpleString("OK");
  }
  if (sub == "setinfo" && argv.size() == 4) return ClientSetInfo(ctx, argv);
  ctx.reply.Error("ERR unknown subcommand or wrong number of arguments for '" + argv[1] +
                  "'. Try CLIENT HELP.");
}

}  // namespace

void RegisterConnectionCommands(CommandRegistry& r) {
  r.Register({"ping", -1, kCmdFast, 0, 0, 0, &Ping});
  r.Register({"echo", 2, kCmdFast, 0, 0, 0, &Echo});
  r.Register({"quit", -1, kCmdNoAuth | kCmdFast, 0, 0, 0, &Quit});
  r.Register({"auth", -2, kCmdNoAuth | kCmdFast | kCmdNoScript, 0, 0, 0, &Auth});
  r.Register({"hello", -1, kCmdNoAuth | kCmdFast | kCmdNoScript, 0, 0, 0, &Hello});
  r.Register({"client", -2, kCmdNoScript, 0, 0, 0, &Client});
}

}  // namespace tideskv
