#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "cluster/cluster_errors.h"
#include "commands/all_commands.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "encoding/key_layout.h"
#include "exec/engine.h"
#include "repl/repl_errors.h"
#include "repl/replication.h"
#include "types/errors.h"

// The replication commands with Redis 7.2's client-visible texts: the master side of the handshake (M6a D8,
// D10: PSYNC, SYNC, REPLCONF; redis72-replication §6), and REPLICAOF / SLAVEOF / ROLE (D14, §1-§2).
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

// PSYNC <replid> <next_seq> [FAILOVER] (syncCommand, rp:940). +CONTINUE and +FULLRESYNC both hand the
// socket to replication once the reply is flushed; after FULLRESYNC the sender streams the snapshot first
// (M6b D2, D4).
void Psync(ExecContext& ctx, const ArgList& argv) {
  Session& s = ctx.session;
  if (s.is_replica_link) return;  // already a replica: ignored, no reply (CLIENT_SLAVE)
  if (argv.size() > 3 && EqualsIgnoreCase(argv[3], "failover")) {
    return ctx.reply.Error(kErrPsyncFailoverToMaster);  // a replica's FAILOVER promotion comes with Task 12
  }
  int64_t offset = 0;
  if (!ParseInt64(argv[2], &offset)) return ctx.reply.Error(kErrNotInteger);
  // Slotless key layout: a replica that declared another layout gets a full resynchronisation, which makes it
  // adopt ours; one that declared none is slotted and cannot (a slotless master refuses it).
  const std::optional<encoding::KeyLayout> layout = s.replica_handshake.key_layout;
  Replication& repl = ctx.server.replication();
  repl::PsyncRequest req{argv[1], offset < 0 ? 0 : static_cast<uint64_t>(offset),
                         layout && *layout != encoding::CurrentKeyLayout(), !layout};
  req.announced_layout = s.replica_handshake.announced_layout;
  const PsyncResult r = repl.HandlePsync(req, PeerOf(s));
  if (!r.error.empty()) return ctx.reply.Error(r.error);  // maxclients (F4), a replica (F8): no hand-off
  if (r.decision == repl::PsyncDecision::kFullResync) {
    ctx.reply.SimpleString("FULLRESYNC " + r.replid + " " + std::to_string(r.seq));
  } else {
    ctx.reply.SimpleString("CONTINUE " + r.replid);
  }
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
  } else if (EqualsIgnoreCase(opt, "key-layout")) {
    const std::optional<encoding::KeyLayout> layout = encoding::ParseKeyLayout(ToLower(val));
    if (!layout) return fail(kErrReplconfKeyLayout);
    hs.key_layout = layout;
    hs.announced_layout = encoding::CurrentKeyLayout();  // what Replconf answers, checked again at PSYNC
    o->key_layout = true;
  } else if (EqualsIgnoreCase(opt, "ack") || EqualsIgnoreCase(opt, "getack")) {
    o->getack = EqualsIgnoreCase(opt, "getack");
    *silent = true;  // a replica's ACK reaches its ReplSender, not this command
  } else {
    return fail(std::string(kErrReplconfUnrecognized) + std::string(opt));
  }
  return true;
}

// REPLCONF <option> <value> ... (replconfCommand, rp:1169): pairwise, +OK once at the end. TidesKV: with
// key-layout among the options the reply is this master's layout (+slotted or +slotless) instead, so the
// replica learns it before PSYNC; a master that predates layouts answers -ERR (and is slotted).
void Replconf(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() % 2 == 0) return ctx.reply.Error(kErrSyntax);
  ReplconfOptions o;
  bool silent = false;
  for (size_t i = 1; i < argv.size() && !silent; i += 2) {
    if (!ApplyReplconf(ctx, argv[i], argv[i + 1], &o, &silent)) return;
  }
  ctx.server.replication().OnReplconf(ctx.session, o);
  if (silent) return;
  ctx.reply.SimpleString(o.key_layout ? encoding::KeyLayoutName(encoding::CurrentKeyLayout()) : "OK");
}

// Runs a role change now, or after the batch inside EXEC: the change joins threads that may wait for the
// Engine gate, which the batch holds exclusively.
void RunRoleChange(ExecContext& ctx, std::function<void()> change) {
  if (ctx.after_batch != nullptr) {
    ctx.after_batch->push_back(std::move(change));
  } else {
    change();
  }
}

// REPLICAOF host port | NO ONE (replicaofCommand, redis72-replication §1.2).
void Replicaof(ExecContext& ctx, const ArgList& argv) {
  if (ctx.server.cluster() != nullptr) return ctx.reply.Error(cluster::kErrReplicaofInCluster);  // D10
  Replication& repl = ctx.server.replication();
  if (EqualsIgnoreCase(argv[1], "no") && EqualsIgnoreCase(argv[2], "one")) {
    if (ctx.server.engine().loading()) return ctx.reply.Error(kErrLoading);  // D7: Redis's answer
    if (ctx.after_batch != nullptr) {  // EXEC already answered +OK; BecomeMaster logs a failure
      ctx.after_batch->push_back([&repl] { (void)repl.BecomeMaster(); });
      return ctx.reply.SimpleString("OK");
    }
    const Status st = repl.BecomeMaster();  // a no-op on a master, still +OK
    if (st.code() == StatusCode::kUserError) return ctx.reply.Error(st.message());  // -LOADING (D22)
    if (!st.ok()) return ctx.reply.Error(std::string(kErrPromotionFailed) + st.message());
    return ctx.reply.SimpleString("OK");
  }
  if (ctx.session.is_replica_link) return ctx.reply.Error(kErrReplicaofFromReplica);
  int64_t port = 0;
  if (!ParseInt64(argv[2], &port) || port < 0 || port > 65535) return ctx.reply.Error(kErrInvalidMasterPort);
  const std::string& host = argv[1];
  const auto p = static_cast<uint16_t>(port);
  if (ctx.after_batch != nullptr) {
    const RoleInfo now = repl.Role();
    if (now.role == ReplRole::kReplica && EqualsIgnoreCase(now.host, host) && now.port == p) {
      return ctx.reply.SimpleString(kReplyAlreadyConnected);
    }
    RunRoleChange(ctx, [&repl, host, p] { repl.BecomeReplica(host, p); });
    return ctx.reply.SimpleString("OK");
  }
  ctx.reply.SimpleString(repl.BecomeReplica(host, p) ? std::string_view("OK") : kReplyAlreadyConnected);
}

// ROLE (roleCommand, redis72-replication §2): a master lists its live replicas past their full sync
// (Redis lists only SLAVE_STATE_ONLINE), with port and offset as bulk strings; a replica reports its link
// state and an offset of -1 while the link is down.
void Role(ExecContext& ctx, const ArgList& /*argv*/) {
  const RoleInfo r = ctx.server.replication().Role();
  resp::ReplyBuilder& out = ctx.reply;
  if (r.role == ReplRole::kReplica) {
    out.ArrayHeader(5);
    out.Bulk("slave");
    out.Bulk(r.host);
    out.Integer(r.port);
    out.Bulk(LinkStateName(r.state));
    out.Integer(r.offset_or_minus1);
    return;
  }
  out.ArrayHeader(3);
  out.Bulk("master");
  out.Integer(static_cast<int64_t>(r.offset));
  const auto listed = [](const ReplicaInfo& i) { return i.alive && i.online; };
  out.ArrayHeader(static_cast<size_t>(std::count_if(r.replicas.begin(), r.replicas.end(), listed)));
  for (const ReplicaInfo& i : r.replicas) {
    if (!listed(i)) continue;
    out.ArrayHeader(3);
    out.Bulk(i.ip);
    out.Bulk(std::to_string(i.listening_port));
    out.Bulk(std::to_string(i.acked));
  }
}

}  // namespace

void RegisterReplicationCommands(CommandRegistry& r) {
  r.Register({"psync", -3, kCmdAdmin | kCmdNoScript | kCmdNoMulti | kCmdLoading, 0, 0, 0, &Psync});
  r.Register({"sync", 1, kCmdAdmin | kCmdNoScript | kCmdNoMulti | kCmdLoading, 0, 0, 0, &Sync});
  r.Register({"replconf", -1, kCmdAdmin | kCmdNoScript | kCmdAllowBusy | kCmdLoading | kCmdStale, 0, 0, 0,
              &Replconf});
  r.Register({"replicaof", 3, kCmdAdmin | kCmdNoScript | kCmdLoading | kCmdStale, 0, 0, 0,
              &Replicaof});  // loading: TidesKV divergence (M6b D7)
  r.Register({"slaveof", 3, kCmdAdmin | kCmdNoScript | kCmdLoading | kCmdStale, 0, 0, 0,
              &Replicaof});  // loading: TidesKV divergence (M6b D7)
  r.Register({"role", 1, kCmdNoScript | kCmdFast | kCmdLoading | kCmdStale, 0, 0, 0, &Role});
}

}  // namespace tideskv
