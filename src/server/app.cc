#include "server/app.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <thread>

#include "commands/all_commands.h"
#include "commands/blocking_serve.h"
#include "common/string_util.h"
#include "common/version.h"
#include "server/connection_block_handler.h"
#include "types/errors.h"
#include "types/format_version.h"

namespace tideskv {
namespace {

SyncMode ToSyncMode(const std::string& s) {
  if (s == "full") return SyncMode::kFull;
  if (s == "none") return SyncMode::kNone;
  return SyncMode::kInterval;
}

spdlog::level::level_enum ToLevel(const std::string& s) {
  if (s == "debug") return spdlog::level::debug;
  if (s == "warning") return spdlog::level::warn;
  return spdlog::level::info;  // verbose, notice
}

int EffectiveWorkers(int configured) {
  return configured > 0 ? configured : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
}

}  // namespace

Status SetupLogging(const ConfigValues& v) {
  try {
    auto logger = v.logfile.empty() ? spdlog::stderr_color_mt("tideskv")
                                    : spdlog::basic_logger_mt("tideskv", v.logfile, /*truncate=*/false);
    logger->set_level(ToLevel(v.loglevel));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    return Status::Ok();
  } catch (const spdlog::spdlog_ex& e) {
    return Status::IOError(std::string("cannot set up logging: ") + e.what());
  }
}

Result<std::unique_ptr<App>> App::Create(std::unique_ptr<Config> config) {
  const ConfigValues v = config->Snapshot();
  std::error_code ec;
  std::filesystem::create_directories(v.dir, ec);
  if (ec) return Status::IOError("cannot create dir " + v.dir + ": " + ec.message());
  StorageOptions so;
  so.path = (std::filesystem::path(v.dir) / "db").string();
  so.sync_mode = ToSyncMode(v.sync_mode);
  so.sync_interval_us = v.sync_interval_us;
  so.block_cache_size = v.block_cache_size;
  so.memtable_size = v.memtable_size;
  so.compression = v.compression == "zstd" ? Compression::kZstd
                   : v.compression == "no" ? Compression::kNone
                                           : Compression::kLz4;
  auto storage = TidesStorage::Open(so);
  if (!storage.ok()) return storage.status();
  if (Status st = EnsureFormatVersion(**storage); !st.ok()) return st;
  std::unique_ptr<App> app(new App(std::move(config), std::move(storage).value()));
  if (Status st = app->repl_log_->Recover(); !st.ok()) return st;  // before any commit (the oid lease)
  if (Status st = app->CreateReplication(); !st.ok()) return st;   // before any commit, like Recover
  if (Status st = app->oids_->Init(); !st.ok()) return st;
  return app;
}

App::App(std::unique_ptr<Config> config, std::unique_ptr<TidesStorage> storage)
    : config_(std::move(config)), storage_(std::move(storage)) {
  repl_log_ = std::make_unique<ReplLog>(*storage_, clock_);
  oids_ = std::make_unique<OidAllocator>(
      *storage_, [this] { return storage_->Checkpoint(); },
      [this](const std::vector<encoding::ReplOp>& ops) { return repl_log_->CommitStandalone(ops); });
  CompactionScheduler::Options co;
  co.collection_delete_threshold = [this] { return config_->Snapshot().compaction_collection_deletes; };
  scheduler_ = std::make_unique<CompactionScheduler>(*storage_, std::move(co));
  engine_ =
      std::make_unique<Engine>(*storage_, *locks_, clock_, scheduler_.get(), &watches_, repl_log_.get());
  engine_->SetBusyGuard([this] { return scripts_.TimedOut(); }, std::string(kErrBusyScript));  // H6
  scripts_.SetOnTimeout([this] { engine_->WakeWaiters(); });  // the script thread holds the gate: no lock
  reaper_ = std::make_unique<ExpireReaper>(*engine_, ExpireReaper::Options{});
  RegisterAllCommands(registry_);
  server_ = std::make_unique<net::Server>(
      MakeServerOptions(),
      [this](Session& session, const std::vector<std::string>& argv, resp::ReplyBuilder& reply) {
        ReadyKeys ready;
        ConnectionBlockHandler block(blocking_, session, argv, ready);
        ExecContext ctx{session, reply, *this, &block, &ready};
        Dispatch(registry_, ctx, argv);
        ServeBlockedClients(registry_, *this, ready);  // before this command's reply leaves (F6)
      });
}

App::~App() { Stop(); }

Status App::CreateReplication() {
  auto identity = repl::LoadOrCreateIdentity(*storage_);
  if (!identity.ok()) return identity.status();
  repl::ReplicationManager::Options o;
  o.repl_timeout = [this] { return std::chrono::milliseconds(config_->Snapshot().repl_timeout * 1000); };
  repl::ReplicationManager::RoleHooks& h = o.role;
  h.engine = engine_.get();
  h.config = [this] { return config_->Snapshot(); };
  h.stop_reaper = [this] { reaper_->Stop(); };    // role changes are serialised by the manager
  h.start_reaper = [this] { reaper_->Start(); };  // D13: the reaper runs only on a master
  h.disconnect_blocked = [this](std::string_view error) { blocking_.DisconnectAll(error); };
  h.reinit_oids = [this] { return oids_->Init(); };
  h.set_replicaof = [this](const std::string& value) {
    if (Status st = config_->Set("replicaof", value.empty() ? "no one" : value, /*at_runtime=*/false);
        !st.ok()) {
      spdlog::warn("cannot record replicaof in the config: {}", st.ToString());
    }
  };
  replication_ = std::make_unique<repl::ReplicationManager>(*storage_, *repl_log_,
                                                            std::move(identity).value(), std::move(o));
  return Status::Ok();
}

// `replicaof` from the config file or --replicaof: "<host> <port>" (validated when it was set).
Status App::StartRole() {
  const std::string replicaof = config_->Snapshot().replicaof;
  if (replicaof.empty()) {
    reaper_->Start();
    return Status::Ok();
  }
  const size_t space = replicaof.rfind(' ');
  uint64_t port = 0;
  if (space == std::string::npos || !ParseUint64(replicaof.substr(space + 1), &port) || port > 65535) {
    return Status::InvalidArgument("replicaof must be '<host> <port>'");
  }
  replication_->BecomeReplica(replicaof.substr(0, space), static_cast<uint16_t>(port));
  return Status::Ok();
}

net::ServerOptions App::MakeServerOptions() {
  const ConfigValues v = config_->Snapshot();
  net::ServerOptions o;
  o.bind = v.bind;
  o.port = v.port;
  o.workers = EffectiveWorkers(v.workers);
  o.maxclients = [this] { return config_->Snapshot().maxclients; };
  o.query_buffer_limit = [this] {
    return static_cast<size_t>(config_->Snapshot().client_query_buffer_limit);
  };
  o.init_session = [this](Session& s) { s.authenticated = config_->Snapshot().requirepass.empty(); };
  o.on_disconnect = [this](Session& s) {
    if (s.watch) watches_.UnwatchAll(*s.watch);
  };
  return o;
}

Status App::Start() {
  // Handle signals before any socket is bound: a SIGTERM arriving while starting up is
  // queued by asio and delivered once Run() arms the wait.
  signals_.emplace(main_ctx_, SIGINT, SIGTERM);
  // A configured replica is fenced before the first client connects, so no write lands as a master.
  if (Status st = StartRole(); !st.ok()) return st;
  if (Status st = server_->Start(); !st.ok()) return st;
  scheduler_->Start();
  replication_->Start();
  return Status::Ok();
}

void App::Run() {
  if (!signals_) signals_.emplace(main_ctx_, SIGINT, SIGTERM);
  signals_->async_wait([this](std::error_code ec, int sig) {
    if (!ec) spdlog::info("received signal {}, shutting down", sig);
    main_ctx_.stop();
  });
  main_ctx_.run();
  Stop();
}

void App::RequestShutdown() {
  asio::post(main_ctx_, [this] {
    spdlog::info("SHUTDOWN requested by client");
    main_ctx_.stop();
  });
}

void App::Stop() {
  if (stopped_) return;
  stopped_ = true;
  if (replication_) replication_->Shutdown();  // replica links first: senders read the replog (M6a)
  scripts_.KillForShutdown();                  // a running script must not hold up the worker threads
  server_->Stop();                             // no command can touch storage after this
  reaper_.reset();                             // joins the reaper thread; it writes through engine_
  engine_.reset();                             // references storage_, so it goes first
  scheduler_.reset();  // compacts storage_; nothing commits (feeds it) once the engine is gone
  oids_.reset();       // likewise (its persist hook checkpoints storage_)
  repl_log_.reset();   // engine_ and oids_ committed through it
  storage_.reset();    // flushes and closes TidesDB
}

ServerStats App::Stats() const {
  const ConfigValues v = config_->Snapshot();
  const net::ServerCounters k = server_->counters();
  ServerStats s;
  s.version = std::string(Version());
  s.pid = static_cast<int64_t>(::getpid());
  s.tcp_port = server_->port();
  s.uptime_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
  s.workers = EffectiveWorkers(v.workers);
  s.maxclients = v.maxclients;
  s.connected_clients = k.connected;
  s.total_connections = k.total_connections;
  s.total_commands = k.total_commands;
  s.data_dir = v.dir;
  s.sync_mode = v.sync_mode;
  s.storage_compression = std::string(CompressionName(storage_->compression()));
  if (auto st = storage_->Stats(); st.ok()) {  // stats are best-effort; never fail INFO
    for (ColumnFamily cf : kAllColumnFamilies) {
      if (cf == ColumnFamily::kReplog) continue;  // the backlog has its own retention (M6a), not compaction's
      s.storage_keys += st->cf[CfIndex(cf)].total_keys;
      s.storage_tombstones += st->cf[CfIndex(cf)].total_tombstones;
    }
    s.storage_flushes = st->flush_count;
    s.storage_compactions = st->compaction_count;
  }
  s.background_compactions = scheduler_ ? scheduler_->completed() : 0;
  s.background_compaction_failures = scheduler_ ? scheduler_->failed() : 0;
  s.expired_keys = reaper_ ? reaper_->expired_total() : 0;
  s.blocked_clients = blocking_.blocked_clients();
  s.blocking_keys = blocking_.blocking_keys();
  s.lua_memory = scripts_.lua_memory();
  s.cached_scripts = scripts_.cached_scripts();
  return s;
}

}  // namespace tideskv
