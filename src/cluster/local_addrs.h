#pragma once
#include <string>
#include <string_view>
#include <vector>

// myid detection (M7a D6): which node of a pushed topology is this process.
namespace tideskv::cluster {

// Every IPv4/IPv6 address of this machine's interfaces, numeric (getifaddrs); empty on error.
std::vector<std::string> LocalInterfaceAddresses();
// Kvrocks util::MatchListeningIP (io_util.cc:394-406): host is the bind address itself, or the bind address
// is a wildcard (0.0.0.0 / ::) and host is one of `local`.
bool MatchListeningIp(std::string_view bind, std::string_view host, const std::vector<std::string>& local);

}  // namespace tideskv::cluster
