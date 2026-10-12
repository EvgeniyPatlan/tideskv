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
#include "common/version.h"
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
  if (Status st = app->oids_->Init(); !st.ok()) return st;
  return app;
}

App::App(std::unique_ptr<Config> config, std::unique_ptr<TidesStorage> storage)
    : config_(std::move(config)), storage_(std::move(storage)) {
  oids_ = std::make_unique<OidAllocator>(*storage_, [this] { return storage_->Checkpoint(); });
  CompactionScheduler::Options co;
  co.collection_delete_threshold = [this] { return config_->Snapshot().compaction_collection_deletes; };
  scheduler_ = std::make_unique<CompactionScheduler>(*storage_, std::move(co));
  engine_ = std::make_unique<Engine>(*storage_, *locks_, clock_, scheduler_.get());
  reaper_ = std::make_unique<ExpireReaper>(*engine_, ExpireReaper::Options{});
  RegisterAllCommands(registry_);
  server_ = std::make_unique<net::Server>(
      MakeServerOptions(),
      [this](Session& session, const std::vector<std::string>& argv, resp::ReplyBuilder& reply) {
        ExecContext ctx{session, reply, *this};
        Dispatch(registry_, ctx, argv);
      });
}

App::~App() { Stop(); }

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
  return o;
}

Status App::Start() {
  // Handle signals before any socket is bound: a SIGTERM arriving while starting up is
  // queued by asio and delivered once Run() arms the wait.
  signals_.emplace(main_ctx_, SIGINT, SIGTERM);
  if (Status st = server_->Start(); !st.ok()) return st;
  reaper_->Start();
  scheduler_->Start();
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
  server_->Stop();     // no command can touch storage after this
  reaper_.reset();     // joins the reaper thread; it writes through engine_
  engine_.reset();     // references storage_, so it goes first
  scheduler_.reset();  // compacts storage_; nothing commits (feeds it) once the engine is gone
  oids_.reset();       // likewise (its persist hook checkpoints storage_)
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
    for (const CfStats& cf : st->cf) {
      s.storage_keys += cf.total_keys;
      s.storage_tombstones += cf.total_tombstones;
    }
    s.storage_flushes = st->flush_count;
    s.storage_compactions = st->compaction_count;
  }
  s.background_compactions = scheduler_ ? scheduler_->completed() : 0;
  s.background_compaction_failures = scheduler_ ? scheduler_->failed() : 0;
  s.expired_keys = reaper_ ? reaper_->expired_total() : 0;
  return s;
}

}  // namespace tideskv
