#include "multinode/cluster_mixed_workload.h"

#include <random>
#include <utility>

#include "common/crc16.h"

namespace tideskv::test::multinode {
namespace {

using Command = std::vector<std::string>;
using Rng = std::mt19937;

int Uniform(Rng& rng, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

// "{tag}<family><n>": the family letter fixes the type (s string, h hash, l list, z zset, t set).
std::string Key(Rng& rng, const std::vector<std::string>& tags, char family) {
  return "{" + tags[static_cast<size_t>(Uniform(rng, 0, static_cast<int>(tags.size()) - 1))] + "}" + family +
         std::to_string(Uniform(rng, 0, 9));
}

std::string Member(Rng& rng) { return "m" + std::to_string(Uniform(rng, 0, 19)); }

Command Collection(Rng& rng, const std::vector<std::string>& tags, std::string* key) {
  switch (Uniform(rng, 0, 7)) {
    case 0:
      *key = Key(rng, tags, 'h');
      return {"HSET", *key, Member(rng), std::to_string(Uniform(rng, 0, 999))};
    case 1:
      *key = Key(rng, tags, 'h');
      return {"HDEL", *key, Member(rng)};
    case 2:
      *key = Key(rng, tags, 'l');
      return {"LPUSH", *key, Member(rng)};
    case 3:
      *key = Key(rng, tags, 'l');
      return {"RPOP", *key};
    case 4:
      *key = Key(rng, tags, 'z');
      return {"ZADD", *key, std::to_string(Uniform(rng, -50, 50)), Member(rng)};
    case 5:
      *key = Key(rng, tags, 'z');
      return {"ZINCRBY", *key, "1.5", Member(rng)};
    case 6:
      *key = Key(rng, tags, 't');
      return {"SADD", *key, Member(rng)};
    default:
      *key = Key(rng, tags, 't');
      return {"SREM", *key, Member(rng)};
  }
}

// One step: a write and the key that routes it. DEL then later writes re-create a collection (a new oid on
// the source, a remap on the target); EXPIRE/PERSIST move expire entries (the derived and forwarded entries).
Command Step(Rng& rng, const std::vector<std::string>& tags, std::string* key) {
  switch (Uniform(rng, 0, 4)) {
    case 0:
      *key = Key(rng, tags, 's');
      return {"SET", *key, std::to_string(Uniform(rng, 0, 1000000)), "EX", "600"};
    case 1:
      *key = Key(rng, tags, "hlzt"[Uniform(rng, 0, 3)]);
      return {"DEL", *key};
    case 2:
      *key = Key(rng, tags, "shlzt"[Uniform(rng, 0, 4)]);
      return Uniform(rng, 0, 1) == 0 ? Command{"EXPIRE", *key, "600"} : Command{"PERSIST", *key};
    default:
      return Collection(rng, tags, key);
  }
}

}  // namespace

std::vector<std::string> TagsInRange(uint16_t first, uint16_t last, size_t n) {
  std::vector<std::string> out;
  for (uint64_t i = 0; out.size() < n; ++i) {
    std::string tag = "t" + std::to_string(i);
    const uint16_t slot = KeySlot(tag);
    if (slot >= first && slot <= last) out.push_back(std::move(tag));
  }
  return out;
}

ClusterMixedWorkload::ClusterMixedWorkload(const std::vector<uint16_t>& seeds, std::vector<std::string> tags,
                                           int clients, uint32_t seed)
    : tags_(std::move(tags)), seed_(seed) {
  for (int i = 0; i < clients; ++i) clients_.push_back(std::make_unique<ClusterClient>(seeds));
}

ClusterMixedWorkload::~ClusterMixedWorkload() { Stop(); }

void ClusterMixedWorkload::Start() {
  stop_ = false;
  ++generation_;
  for (size_t i = 0; i < clients_.size(); ++i) {
    const uint32_t seed = seed_ + generation_ * 1000 + static_cast<uint32_t>(i);
    threads_.emplace_back([this, client = clients_[i].get(), seed] { Run(*client, seed); });
  }
}

void ClusterMixedWorkload::Stop() {
  stop_ = true;
  for (auto& t : threads_) t.join();
  threads_.clear();
}

void ClusterMixedWorkload::Run(ClusterClient& client, uint32_t seed) {
  Rng rng(seed);
  while (!stop_.load()) {
    std::string key;
    const Command cmd = Step(rng, tags_, &key);
    const RespValue r = client.Command(cmd, key);
    steps_.fetch_add(1);
    if (r.type != RespValue::Type::kError) continue;
    failures_.fetch_add(1);
    std::lock_guard lock(mu_);
    ++errors_[r.str];
  }
}

std::map<std::string, uint64_t> ClusterMixedWorkload::errors() const {
  std::lock_guard lock(mu_);
  return errors_;
}

}  // namespace tideskv::test::multinode
