// tideskv-server [config-file] [--option value ...]
// Exit codes: 0 clean shutdown, 1 configuration error, 2 storage open failure, 3 listen failure.
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "common/version.h"
#include "server/app.h"

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  auto config = std::make_unique<tideskv::Config>();
  if (!args.empty() && args[0].rfind("--", 0) != 0) {
    if (const tideskv::Status st = config->LoadFile(args[0]); !st.ok()) {
      std::fprintf(stderr, "configuration error: %s\n", st.message().c_str());
      return 1;
    }
    args.erase(args.begin());
  }
  // Precedence: config file < environment < command-line args.
  std::vector<std::string> ignored_env;
  if (const tideskv::Status st = config->ApplyEnv([](const char* n) { return std::getenv(n); }, &ignored_env);
      !st.ok()) {
    std::fprintf(stderr, "configuration error: %s\n", st.message().c_str());
    return 1;
  }
  if (const tideskv::Status st = config->ApplyArgs(args); !st.ok()) {
    std::fprintf(stderr, "configuration error: %s\n", st.message().c_str());
    return 1;
  }
  if (const tideskv::Status st = tideskv::SetupLogging(config->Snapshot()); !st.ok()) {
    std::fprintf(stderr, "%s\n", st.message().c_str());
    return 1;
  }
  for (const auto& var : ignored_env) spdlog::warn("{} is set but empty; ignored", var);
  auto app = tideskv::App::Create(std::move(config));
  if (!app.ok()) {
    spdlog::critical("cannot start: {}", app.status().message());
    return 2;
  }
  if (const tideskv::Status st = (*app)->Start(); !st.ok()) {
    spdlog::critical("cannot listen: {}", st.message());
    return 3;
  }
  spdlog::info("TidesKV {} ready to accept connections on port {}", tideskv::Version(), (*app)->port());
  (*app)->Run();
  spdlog::info("TidesKV stopped");
  return 0;
}
