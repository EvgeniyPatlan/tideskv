#include <cstdint>
#include <string>
#include <string_view>

#include "commands/all_commands.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "repl/repl_errors.h"
#include "repl/replication.h"
#include "types/errors.h"

// The master side of the replication handshake (M6a D8, D10): PSYNC, SYNC and REPLCONF, with Redis 7.2's
// client-visible texts (redis72-replication §6). REPLICAOF and ROLE arrive with Task 12.
namespace tideskv {
namespace {

// REPLCONF ip-address, else the host part of the connection's "ip:port".
ReplicaPeer PeerOf(const Session& s) {
  ReplicaPeer p;
  p.listening_port = static_cast<uint16_t>(s.replica_handshake.listening_port);
  if (!s.replica_handshake.ip_address.empty()) {
    p.ip = s.replica_handshake.ip_address;
  } else {
    p.ip = s.peer.substr(0, s.peer.rfind(':'));
  }
  return p;
}

// PSYNC <replid> <next_seq> [FAILOVER] (syncCommand, rp:940). +CONTINUE hands the socket to replication
// once the reply is flushed; +FULLRESYNC closes the link in M6a (M6b sends the data).
void Psync(ExecContext& ctx, const ArgList& argv) {
  Session& s = ctx.session;
  if (s.is_replica_link) return;  // already a replica: ignored, no reply (CLIENT_SLAVE)
  if (argv.size() > 3 && EqualsIgnoreCase(argv[3], "failover")) {
    return ctx.reply.Error(kErrPsyncFailoverToMaster);  // a replica's FAILOVER promotion comes with Task 12
  }
  int64_t offset = 0;
  if (!ParseInt64(argv[2], &offset)) return ctx.reply.Error(kErrNotInteger);
  Replication& repl = ctx.server.replication();
  const repl::PsyncRequest req{argv[1], offset < 0 ? 0 : static_cast<uint64_t>(offset)};
  const PsyncResult r = repl.HandlePsync(req, PeerOf(s));
  if (r.decision == repl::PsyncDecision::kFullResync) {
    ctx.reply.SimpleString("FULLRESYNC " + r.replid + " " + std::to_string(r.seq));
    s.close_after_reply = true;
    return;
  }
  ctx.reply.SimpleString("CONTINUE " + r.replid);
  s.is_replica_link = true;
  s.takeover = [&repl, id = r.pending_id](int fd, std::string unread) {
    repl.AttachReplica(id, fd, std::move(unread));
  };
}

void Sync(ExecContext& ctx, const ArgList& /*argv*/) {
  if (ctx.session.is_replica_link) return;
  ctx.reply.Error(kErrSyncNotSupported);  // TidesKV streams no RDB (redis72-replication §6)
}

// Applies one option pair to the session; false after replying an error. *silent: ACK / GETACK, which
// end the command with no reply at all.
bool ApplyReplconf(ExecContext& ctx, std::string_view opt, const std::string& val, ReplconfOptions* o,
                   bool* silent) {
  ReplicaHandshake& hs = ctx.session.replica_handshake;
  const auto fail = [&ctx](std::string_view error) {
    ctx.reply.Error(error);
    return false;
  };
  if (EqualsIgnoreCase(opt, "listening-port")) {
    int64_t port = 0;
    if (!ParseInt64(val, &port)) return fail(kErrNotInteger);
    hs.listening_port = port;
    o->listening_port = port;
  } else if (EqualsIgnoreCase(opt, "ip-address")) {
    if (val.size() > kReplconfMaxIpLength) {
      return fail(std::string(kErrReplconfIpTooLong) + std::to_string(val.size()) + " bytes");
    }
    hs.ip_address = val;
    o->ip_address = val;
  } else if (EqualsIgnoreCase(opt, "capa")) {
    o->capa.push_back(val);  // eof and psync2 are recognised; others are ignored, as in Redis
  } else if (EqualsIgnoreCase(opt, "ack") || EqualsIgnoreCase(opt, "getack")) {
    o->getack = EqualsIgnoreCase(opt, "getack");
    *silent = true;  // a replica's ACK reaches its ReplSender, not this command
  } else {
    return fail(std::string(kErrReplconfUnrecognized) + std::string(opt));
  }
  return true;
}

// REPLCONF <option> <value> ... (replconfCommand, rp:1169): pairwise, +OK once at the end.
void Replconf(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() % 2 == 0) return ctx.reply.Error(kErrSyntax);
  ReplconfOptions o;
  bool silent = false;
  for (size_t i = 1; i < argv.size() && !silent; i += 2) {
    if (!ApplyReplconf(ctx, argv[i], argv[i + 1], &o, &silent)) return;
  }
  ctx.server.replication().OnReplconf(ctx.session, o);
  if (!silent) ctx.reply.SimpleString("OK");
}

}  // namespace

void RegisterReplicationCommands(CommandRegistry& r) {
  r.Register({"psync", -3, kCmdAdmin | kCmdNoScript | kCmdNoMulti, 0, 0, 0, &Psync});
  r.Register({"sync", 1, kCmdAdmin | kCmdNoScript | kCmdNoMulti, 0, 0, 0, &Sync});
  r.Register({"replconf", -1, kCmdAdmin | kCmdNoScript | kCmdAllowBusy, 0, 0, 0, &Replconf});
}

}  // namespace tideskv
