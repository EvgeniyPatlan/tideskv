#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "cluster/topology.h"
#include "common/status.h"
#include "storage/storage.h"

// sys/cluster_cfg (M7a D20): node-local, written in its own direct txn (never through the replog: myid
// differs per node, and a full sync neither streams nor wipes it). Layout:
//   [format u8 = 1][version i64 BE][myid length u8][myid][nodes text in SETNODES format, empty if version <
//   0]
namespace tideskv::cluster {

inline constexpr std::string_view kSysClusterCfg = "cluster_cfg";
inline constexpr uint8_t kClusterCfgFormat = 1;

std::string EncodeClusterCfg(const Topology& t);
Result<std::shared_ptr<Topology>> DecodeClusterCfg(std::string_view raw);  // Corruption on any defect
Result<std::shared_ptr<Topology>> LoadClusterCfg(Storage& storage);  // absent: an uninitialised topology
Status SaveClusterCfg(Storage& storage, const Topology& t);
Status DeleteClusterCfg(Storage& storage);

}  // namespace tideskv::cluster
