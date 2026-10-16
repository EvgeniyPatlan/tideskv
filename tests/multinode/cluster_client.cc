#include "multinode/cluster_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <numeric>
#include <optional>
#include <utility>

namespace tideskv::test::multinode {
namespace {

using namespace std::chrono_literals;

bool StartsWith(const std::string& s, std::string_view p) { return s.rfind(p, 0) == 0; }

bool Retryable(const std::string& error) {
  for (std::string_view p : {"CLUSTERDOWN", "READONLY", "LOADING", "TRYAGAIN", "MASTERDOWN"}) {
    if (StartsWith(error, p)) return true;
  }
  return false;
}

uint16_t MovedPort(const std::string& moved) {  // "MOVED <slot> <host>:<port>"
  return static_cast<uint16_t>(std::stoi(moved.substr(moved.rfind(':') + 1)));
}

}  // namespace

std::string NodesString(const std::vector<const TestServer*>& servers, const std::vector<NodeLine>& lines) {
  std::string out;
  for (const NodeLine& l : lines) {
    out += NodeId(l.index) + " 127.0.0.1 " + std::to_string(servers[l.index]->port());
    out += l.master < 0 ? " master - " + l.slots : " slave " + NodeId(static_cast<size_t>(l.master));
    out += "\n";
  }
  return out;
}

void PushTopology(const std::vector<const TestServer*>& servers, const std::string& nodes, int64_t version,
                  std::vector<size_t> order) {
  if (order.empty()) {
    order.resize(servers.size());
    std::iota(order.begin(), order.end(), 0);
  }
  for (size_t i : order) {
    auto c = servers[i]->Connect();
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->Command({"CLUSTERX", "SETNODEID", NodeId(i)}).str, "OK") << "node " << i;
    ASSERT_EQ(c->Command({"CLUSTERX", "SETNODES", nodes, std::to_string(version)}).str, "OK") << "node " << i;
  }
}

ClusterClient::ClusterClient(std::vector<uint16_t> seeds) : seeds_(std::move(seeds)) {}

RespClient* ClusterClient::ConnectionTo(uint16_t port) {
  if (auto it = conns_.find(port); it != conns_.end()) return it->second.get();
  auto c = RespClient::Connect(port, 5s);
  if (c == nullptr) return nullptr;
  return (conns_[port] = std::move(c)).get();
}

bool ClusterClient::Refresh() {
  std::vector<uint16_t> ports = seeds_;
  for (const auto& [port, conn] : conns_) ports.push_back(port);
  for (uint16_t port : ports) {
    RespClient* c = ConnectionTo(port);
    std::optional<RespValue> r;
    if (c != nullptr && c->Send({"CLUSTER", "SLOTS"})) r = c->Read();
    if (!r) {
      conns_.erase(port);
      continue;
    }
    if (r->type != RespValue::Type::kArray || r->elements.empty()) continue;
    for (const RespValue& run : r->elements) {
      const auto owner = static_cast<uint16_t>(run.elements.at(2).elements.at(1).integer);
      for (int64_t s = run.elements.at(0).integer; s <= run.elements.at(1).integer; ++s)
        owner_port_[s] = owner;
    }
    return true;
  }
  return false;
}

RespValue ClusterClient::Command(const std::vector<std::string>& argv, const std::string& key) {
  const uint16_t slot = KeySlot(key);
  RespValue last;
  last.type = RespValue::Type::kError;
  last.str = "no attempt succeeded";
  int consecutive_moved = 0;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (owner_port_[slot] == 0 && !Refresh()) {
      std::this_thread::sleep_for(20ms);
      continue;
    }
    const uint16_t port = owner_port_[slot];
    RespClient* c = ConnectionTo(port);
    std::optional<RespValue> r;
    if (c != nullptr && c->Send(argv)) r = c->Read();
    if (!r) {  // lost connection or a reply timeout: forget the owner, retry (SET is idempotent here)
      conns_.erase(port);
      owner_port_[slot] = 0;
      std::this_thread::sleep_for(20ms);
      continue;
    }
    last = *r;
    if (r->type != RespValue::Type::kError) return *r;
    if (StartsWith(r->str, "MOVED ")) {
      ++redirects_;
      owner_port_[slot] = MovedPort(r->str);
      // A push reaches the nodes one by one: two of them may point at each other for a moment. Back off
      // after a few MOVEDs in a row so that window cannot use up every attempt.
      if (++consecutive_moved >= kMovedBeforeBackoff) std::this_thread::sleep_for(20ms);
      continue;
    }
    consecutive_moved = 0;
    if (!Retryable(r->str)) return *r;
    owner_port_[slot] = 0;
    std::this_thread::sleep_for(20ms);
  }
  return last;
}

ClusterWorkload::ClusterWorkload(const std::vector<uint16_t>& seeds, int clients) {
  for (int i = 0; i < clients; ++i) clients_.push_back(std::make_unique<ClusterClient>(seeds));
}

ClusterWorkload::~ClusterWorkload() { Stop(); }

void ClusterWorkload::Start() {
  stop_ = false;
  for (auto& c : clients_) threads_.emplace_back([this, client = c.get()] { Run(*client); });
}

void ClusterWorkload::Stop() {
  stop_ = true;
  for (auto& t : threads_) t.join();
  threads_.clear();
}

void ClusterWorkload::Run(ClusterClient& client) {
  while (!stop_.load()) {
    const uint64_t n = next_.fetch_add(1);
    const std::string key = "ack:" + std::to_string(n);
    const RespValue r = client.Command({"SET", key, std::to_string(n)}, key);
    if (r.type == RespValue::Type::kSimple && r.str == "OK") {
      std::lock_guard lock(mu_);
      acked_.push_back(n);
    } else {
      failures_.fetch_add(1);
    }
  }
}

std::vector<uint64_t> ClusterWorkload::acked() const {
  std::lock_guard lock(mu_);
  return acked_;
}

uint64_t ClusterWorkload::redirects() const {  // read while stopped
  uint64_t n = 0;
  for (const auto& c : clients_) n += c->redirects();
  return n;
}

void ExpectAllAcked(const std::vector<uint16_t>& seeds, const std::vector<uint64_t>& acked) {
  ClusterClient reader(seeds);
  int missing = 0;
  for (uint64_t n : acked) {
    const std::string key = "ack:" + std::to_string(n);
    const RespValue r = reader.Command({"GET", key}, key);
    if (r.str == std::to_string(n)) continue;
    ADD_FAILURE() << "acknowledged write " << key << " lost: " << ToDebugString(r);
    if (++missing == 5) return;
  }
}

}  // namespace tideskv::test::multinode
