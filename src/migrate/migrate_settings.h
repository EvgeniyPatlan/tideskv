#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

// What one migration job reads from the configuration (M7b D3, D17, D25), re-read before every chunk. App
// fills it from ConfigValues (migrate/ does not include server/, D26).
namespace tideskv::migrate {

struct MigrateSettings {
  uint64_t sequence_gap = 10000;                   // migrate-sequence-gap
  size_t batch_bytes = size_t{16} * 1024;          // migrate-batch-size-kb
  uint64_t rate_bytes_per_s = uint64_t{16} << 20;  // migrate-batch-rate-limit-mb
  std::chrono::milliseconds timeout{60000};        // repl-timeout: every connect, read and write (D3, D5)
  std::string requirepass;             // AUTH on the destination: this node's own password (KM §5)
  std::chrono::milliseconds delay{0};  // migrate-delay-ms (hidden, D25)
};

}  // namespace tideskv::migrate
