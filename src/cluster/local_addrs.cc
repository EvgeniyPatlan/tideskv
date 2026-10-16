#include "cluster/local_addrs.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>

namespace tideskv::cluster {

std::vector<std::string> LocalInterfaceAddresses() {
  std::vector<std::string> out;
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return out;
  for (const ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
    if (a->ifa_addr == nullptr) continue;
    const int family = a->ifa_addr->sa_family;
    const void* raw = nullptr;
    if (family == AF_INET) raw = &reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr;     // NOLINT
    if (family == AF_INET6) raw = &reinterpret_cast<const sockaddr_in6*>(a->ifa_addr)->sin6_addr;  // NOLINT
    char text[INET6_ADDRSTRLEN] = {};
    if (raw != nullptr && ::inet_ntop(family, raw, text, sizeof(text)) != nullptr) out.emplace_back(text);
  }
  ::freeifaddrs(list);
  return out;
}

bool MatchListeningIp(std::string_view bind, std::string_view host, const std::vector<std::string>& local) {
  if (host == bind) return true;
  if (bind != "0.0.0.0" && bind != "::") return false;
  return std::find(local.begin(), local.end(), host) != local.end();
}

}  // namespace tideskv::cluster
