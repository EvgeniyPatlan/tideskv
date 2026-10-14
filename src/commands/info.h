#pragma once
#include <string>
#include <vector>

#include "commands/server_services.h"

namespace tideskv {

// Redis-style INFO text. Empty / "default" / "all" / "everything" selects every section.
std::string FormatInfo(const ServerStats& stats, const std::vector<std::string>& sections);

// The `# Replication` section (redis72-replication §3.1, offsets are seqs).
std::string FormatReplicationSection(const ReplicationStats& r);

}  // namespace tideskv
