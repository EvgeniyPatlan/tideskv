#include <map>
#include <set>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/info.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "script/script_engine.h"
#include "spdlog/spdlog.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using Fields = std::vector<std::pair<std::string, std::string>>;

std::string Section(const std::string& title, const Fields& fields) {
  std::string out = "# " + title + "\r\n";
  for (const auto& [k, v] : fields) {
    out += k;
    out += ':';
    out += v;
    out += "\r\n";
  }
  return out;
}

// loading, async_loading, the TidesKV fields, then (only while loading) the progress fields
// (server.c:5754, 5859-5870). total_bytes and rdb_used_mem are 0 on a stream: the percentage is 0.00.
Fields PersistenceFields(const ServerStats& s) {
  Fields f = {{"loading", s.loading ? "1" : "0"},
              {"async_loading", "0"},
              {"storage_engine", "tidesdb"},
              {"dir", s.data_dir},
              {"sync_mode", s.sync_mode},
              {"compression", s.storage_compression},
              {"sstable_keys", std::to_string(s.storage_keys)},
              {"tombstones", std::to_string(s.storage_tombstones)},
              {"flushes", std::to_string(s.storage_flushes)},
              {"compactions", std::to_string(s.storage_compactions)},
              {"background_compactions", std::to_string(s.background_compactions)},
              {"background_compaction_failures", std::to_string(s.background_compaction_failures)}};
  if (!s.loading) return f;
  f.insert(f.end(), {{"loading_start_time", std::to_string(s.loading_start_time)},
                     {"loading_total_bytes", "0"},
                     {"loading_rdb_used_mem", "0"},
                     {"loading_loaded_bytes", std::to_string(s.loading_loaded_bytes)},
                     {"loading_loaded_perc", "0.00"},
                     {"loading_eta_seconds", std::to_string(s.loading_eta_seconds)}});
  return f;
}

std::string RenderSection(const std::string& name, const ServerStats& s) {
  if (name == "server") {
    return Section("Server", {{"redis_version", std::string(kRedisCompatVersion)},
                              {"tideskv_version", s.version},
                              {"redis_mode", "standalone"},
                              {"arch_bits", "64"},
                              {"process_id", std::to_string(s.pid)},
                              {"tcp_port", std::to_string(s.tcp_port)},
                              {"uptime_in_seconds", std::to_string(s.uptime_seconds)},
                              {"uptime_in_days", std::to_string(s.uptime_seconds / 86400)},
                              {"workers", std::to_string(s.workers)}});
  }
  if (name == "clients") {
    return Section("Clients", {{"connected_clients", std::to_string(s.connected_clients)},
                               {"maxclients", std::to_string(s.maxclients)},
                               {"blocked_clients", std::to_string(s.blocked_clients)},
                               {"total_blocking_keys", std::to_string(s.blocking_keys)}});
  }
  if (name == "memory") {
    return Section("Memory", {{"used_memory_lua", std::to_string(s.lua_memory)},
                              {"number_of_cached_scripts", std::to_string(s.cached_scripts)}});
  }
  if (name == "stats") {
    return Section("Stats", {{"total_connections_received", std::to_string(s.total_connections)},
                             {"total_commands_processed", std::to_string(s.total_commands)},
                             {"expired_keys", std::to_string(s.expired_keys)},
                             {"sync_full", std::to_string(s.repl.sync_full)},
                             {"sync_partial_ok", std::to_string(s.repl.sync_partial_ok)},
                             {"sync_partial_err", std::to_string(s.repl.sync_partial_err)}});
  }
  if (name == "persistence") return Section("Persistence", PersistenceFields(s));
  if (name == "replication") return FormatReplicationSection(s.repl);
  if (name == "keyspace") return Section("Keyspace", {{"sequence", std::to_string(s.repl.applied_seq)}});
  return "";
}

const std::vector<std::string>& AllSections() {
  static const std::vector<std::string> kAll = {"server",      "clients",     "memory",  "stats",
                                                "persistence", "replication", "keyspace"};
  return kAll;
}

void ArityError(ExecContext& ctx, std::string_view name) {
  ctx.reply.Error("ERR wrong number of arguments for '" + std::string(name) + "' command");
}

void UnknownSub(ExecContext& ctx, const std::string& sub, std::string_view cmd) {
  ctx.reply.Error("ERR unknown subcommand or wrong number of arguments for '" + sub + "'. Try " +
                  std::string(cmd) + " HELP.");
}

void Info(ExecContext& ctx, const ArgList& argv) {
  ctx.reply.Verbatim(FormatInfo(ctx.server.Stats(), {argv.begin() + 1, argv.end()}));
}

void ConfigGet(ExecContext& ctx, const ArgList& argv) {
  std::map<std::string, std::string> found;  // dedupe across patterns, sorted
  for (size_t i = 2; i < argv.size(); ++i) {
    for (auto& [k, v] : ctx.server.config().Get(argv[i])) found.emplace(std::move(k), std::move(v));
  }
  ctx.reply.MapHeader(found.size());
  for (const auto& [k, v] : found) {
    ctx.reply.Bulk(k);
    ctx.reply.Bulk(v);
  }
}

void ConfigSet(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() < 4 || argv.size() % 2 != 0) return ArityError(ctx, "config|set");
  Config::Pairs pairs;
  for (size_t i = 2; i + 1 < argv.size(); i += 2) pairs.emplace_back(argv[i], argv[i + 1]);
  std::string failed;
  const Status st = ctx.server.config().SetMany(pairs, /*at_runtime=*/true, &failed);
  if (!st.ok()) {
    return ctx.reply.Error("ERR CONFIG SET failed (possibly related to argument '" + failed + "') - " +
                           st.message());
  }
  ctx.reply.SimpleString("OK");
}

void ConfigCmd(ExecContext& ctx, const ArgList& argv) {
  const std::string sub = ToLower(argv[1]);
  if (sub == "get" && argv.size() >= 3) return ConfigGet(ctx, argv);
  if (sub == "set") return ConfigSet(ctx, argv);
  if (sub == "resetstat" && argv.size() == 2) {
    ctx.server.ResetStats();
    return ctx.reply.SimpleString("OK");
  }
  if (sub == "rewrite" && argv.size() == 2) {
    const Status st = ctx.server.config().Rewrite();
    if (st.ok()) return ctx.reply.SimpleString("OK");
    if (st.code() == StatusCode::kInvalidArgument) {
      return ctx.reply.Error("ERR " + st.message());
    }
    // Log the detail server-side; don't expose paths in reply (spec §5.4)
    spdlog::error("CONFIG REWRITE failed: {}", st.message());
    return ctx.reply.Error("ERR CONFIG REWRITE failed");
  }
  UnknownSub(ctx, argv[1], "CONFIG");
}

void WriteCommandInfo(resp::ReplyBuilder& r, const CommandSpec& spec) {
  r.ArrayHeader(6);
  r.Bulk(spec.name);
  r.Integer(spec.arity);
  const auto flags = FlagNames(spec.flags);
  r.SetHeader(flags.size());
  for (std::string_view f : flags) r.SimpleString(f);
  r.Integer(spec.first_key);
  r.Integer(spec.last_key);
  r.Integer(spec.key_step);
}

void CommandCmd(ExecContext& ctx, const ArgList& argv) {
  const auto& all = ctx.server.registry().all();
  const std::string sub = argv.size() > 1 ? ToLower(argv[1]) : "";
  if (argv.size() == 1) {
    ctx.reply.ArrayHeader(all.size());
    for (const auto& [name, spec] : all) WriteCommandInfo(ctx.reply, spec);
  } else if (sub == "count" && argv.size() == 2) {
    ctx.reply.Integer(static_cast<int64_t>(all.size()));
  } else if (sub == "list" && argv.size() == 2) {
    ctx.reply.ArrayHeader(all.size());
    for (const auto& [name, spec] : all) ctx.reply.Bulk(name);
  } else if (sub == "info" && argv.size() >= 3) {
    ctx.reply.ArrayHeader(argv.size() - 2);
    for (size_t i = 2; i < argv.size(); ++i) {
      const CommandSpec* spec = ctx.server.registry().Find(argv[i]);
      spec != nullptr ? WriteCommandInfo(ctx.reply, *spec) : ctx.reply.NullArray();
    }
  } else if (sub == "docs") {
    ctx.reply.MapHeader(0);  // docs not provided; redis-cli tolerates an empty map
  } else {
    UnknownSub(ctx, argv[1], "COMMAND");
  }
}

void Shutdown(ExecContext& ctx, const ArgList& argv) {
  bool nosave = false;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string opt = ToLower(argv[i]);
    if (opt == "abort") return ctx.reply.Error("ERR No shutdown in progress.");
    if (opt != "nosave" && opt != "save" && opt != "now" && opt != "force")
      return ctx.reply.Error("ERR syntax error");
    nosave = nosave || opt == "nosave";
  }
  if (ctx.server.scripts().TimedOut()) {
    if (!nosave) return ctx.reply.Error(kErrBusyScript);  // only SHUTDOWN NOSAVE (db.c:1222)
    ctx.server.scripts().KillForShutdown();               // the script dies at its next hook
  }
  ctx.server.RequestShutdown();
  ctx.session.close_after_reply = true;  // like Redis: no reply, the connection just closes
}

}  // namespace

std::string FormatInfo(const ServerStats& stats, const std::vector<std::string>& sections) {
  std::vector<std::string> wanted;
  bool all = sections.empty();
  for (const std::string& s : sections) {
    std::string name = ToLower(s);
    if (name == "default" || name == "all" || name == "everything") all = true;
    wanted.push_back(std::move(name));
  }
  if (all) wanted = AllSections();
  std::set<std::string> seen;  // each section printed at most once, first-request order
  std::string out;
  for (const std::string& name : wanted) {
    if (!seen.insert(name).second) continue;
    const std::string section = RenderSection(name, stats);
    if (section.empty()) continue;
    if (!out.empty()) out += "\r\n";
    out += section;
  }
  return out;
}

void RegisterServerCommands(CommandRegistry& r) {
  r.Register({"info", -1, kCmdLoading | kCmdStale, 0, 0, 0, &Info});
  r.Register({"config", -2, kCmdAdmin | kCmdNoScript | kCmdLoading | kCmdStale, 0, 0, 0, &ConfigCmd});
  r.Register({"command", -1, kCmdLoading | kCmdStale, 0, 0, 0, &CommandCmd});
  r.Register({"shutdown", -1,
              kCmdAdmin | kCmdNoScript | kCmdNoMulti | kCmdAllowBusy | kCmdLoading | kCmdStale, 0, 0, 0,
              &Shutdown});
}

}  // namespace tideskv
