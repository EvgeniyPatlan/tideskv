#include "multinode/workload.h"

#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>

#include "support/resp_client.h"

namespace tideskv::test {
namespace {

using Command = std::vector<std::string>;
using Rng = std::mt19937;

constexpr int kKeySpace = 500;
// A key's family fixes its type, so most commands hit a key of the right type; RENAME stays within one.
enum Family : uint8_t { kString, kCounter, kHash, kList, kSet, kZset, kFamilies };
constexpr int kMaxTtlSeconds = 5;  // short: keys expire and get reaped while the load runs

// One step of a client: commands sent in one write, and the n of an `ack:<n>` SET among them.
struct Op {
  std::vector<Command> commands;
  std::optional<uint64_t> ack;
};

int Uniform(Rng& rng, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

std::string Key(Rng& rng, Family f) {
  const int slots = (kKeySpace - 1 - f) / kFamilies;
  return "key:" + std::to_string(f + kFamilies * Uniform(rng, 0, slots));
}

std::string AnyKey(Rng& rng) { return "key:" + std::to_string(Uniform(rng, 0, kKeySpace - 1)); }

std::string Value(Rng& rng) {
  return std::string(static_cast<size_t>(Uniform(rng, 1, 32)), static_cast<char>('a' + Uniform(rng, 0, 25)));
}

std::string Member(Rng& rng) { return "m" + std::to_string(Uniform(rng, 0, 19)); }

std::string Ttl(Rng& rng) { return std::to_string(Uniform(rng, 1, kMaxTtlSeconds)); }

Family Collection(Rng& rng) { return static_cast<Family>(Uniform(rng, kHash, kZset)); }

Command StringWrite(Rng& rng, bool expiry) {
  switch (Uniform(rng, 0, 3)) {
    case 0:
      return expiry ? Command{"SET", Key(rng, kString), Value(rng), "EX", Ttl(rng)}
                    : Command{"SET", Key(rng, kString), Value(rng)};
    case 1:  // GETSET with EX
      return expiry ? Command{"SET", Key(rng, kString), Value(rng), "GET", "EX", Ttl(rng)}
                    : Command{"GETSET", Key(rng, kString), Value(rng)};
    case 2:
      return {"APPEND", Key(rng, kString), Value(rng)};
    default:
      return {"INCR", Key(rng, kCounter)};
  }
}

Command CollectionWrite(Rng& rng) {
  switch (Uniform(rng, 0, 10)) {
    case 0:
      return {"HSET", Key(rng, kHash), "f" + std::to_string(Uniform(rng, 0, 9)), Value(rng)};
    case 1:
      return {"HDEL", Key(rng, kHash), "f" + std::to_string(Uniform(rng, 0, 9))};
    case 2:
      return {"LPUSH", Key(rng, kList), Value(rng)};
    case 3:
      return {"RPOP", Key(rng, kList)};
    case 4:
      return {"LMOVE", Key(rng, kList), Key(rng, kList), "LEFT", "RIGHT"};
    case 5:
      return {"SADD", Key(rng, kSet), Member(rng)};
    case 6:
      return {"SREM", Key(rng, kSet), Member(rng)};
    case 7:
      return {"SMOVE", Key(rng, kSet), Key(rng, kSet), Member(rng)};
    case 8:
      return {"ZADD", Key(rng, kZset), std::to_string(Uniform(rng, 0, 100)), Member(rng)};
    case 9:
      return {"ZINCRBY", Key(rng, kZset), std::to_string(Uniform(rng, 1, 5)), Member(rng)};
    default:
      return {"ZPOPMIN", Key(rng, kZset)};
  }
}

Command KeyWrite(Rng& rng, bool expiry) {
  const int pick = Uniform(rng, 0, expiry ? 3 : 1);
  if (pick == 0) return {"DEL", Key(rng, Collection(rng)), Key(rng, Collection(rng))};
  if (pick == 1) {
    const auto f = static_cast<Family>(Uniform(rng, 0, kFamilies - 1));
    return {"RENAME", Key(rng, f), Key(rng, f)};
  }
  if (pick == 2) return {"EXPIRE", AnyKey(rng), Ttl(rng)};
  return {"PERSIST", AnyKey(rng)};
}

// A single write command: 40% strings, 45% collections, 15% whole keys.
Command SingleWrite(Rng& rng, bool expiry) {
  const int roll = Uniform(rng, 0, 99);
  if (roll < 40) return StringWrite(rng, expiry);
  if (roll < 85) return CollectionWrite(rng);
  return KeyWrite(rng, expiry);
}

Op MultiExec(Rng& rng, bool expiry) {
  Op op;
  op.commands.push_back({"MULTI"});
  const int n = Uniform(rng, 2, 5);
  for (int i = 0; i < n; ++i) op.commands.push_back(SingleWrite(rng, expiry));
  op.commands.push_back({"EXEC"});
  return op;
}

Op EvalThreeKeys(Rng& rng) {
  static constexpr const char* kScript =
      "redis.call('SET', KEYS[1], ARGV[1]) redis.call('INCR', KEYS[2]) "
      "redis.call('HSET', KEYS[3], 'f', ARGV[1]) return 1";
  return {{{"EVAL", kScript, "3", Key(rng, kString), Key(rng, kCounter), Key(rng, kHash), Value(rng)}}, {}};
}

// db 2 gets a few hashes, and now and then loses them all to FLUSHDB.
Op Db2(Rng& rng) {
  Op op;
  op.commands.push_back({"SELECT", "2"});
  op.commands.push_back({"HSET", "d2:" + std::to_string(Uniform(rng, 0, 19)), "f", Value(rng)});
  if (Uniform(rng, 0, 3) == 0) op.commands.push_back({"FLUSHDB"});
  op.commands.push_back({"SELECT", "0"});
  return op;
}

Op NextOp(Rng& rng, bool expiry, std::atomic<uint64_t>& next_ack) {
  const int roll = Uniform(rng, 0, 999);
  if (roll < 50) {
    const uint64_t n = next_ack.fetch_add(1, std::memory_order_relaxed);
    return {{{"SET", "ack:" + std::to_string(n), "v"}}, n};
  }
  if (roll < 120) return MultiExec(rng, expiry);
  if (roll < 160) return EvalThreeKeys(rng);
  if (roll < 165) return Db2(rng);
  return {{SingleWrite(rng, expiry)}, {}};
}

std::string Encode(const std::vector<Command>& commands) {
  std::string out;
  for (const Command& c : commands) out += EncodeCommand(c);
  return out;
}

}  // namespace

Workload::Workload(uint16_t port, uint32_t seed, int clients, Mix mix)
    : port_(port), seed_(seed), clients_(clients), mix_(mix) {}

Workload::~Workload() { Stop(); }

void Workload::Start() {
  Stop();
  stop_.store(false, std::memory_order_release);
  for (int i = 0; i < clients_; ++i) {
    const uint32_t client_seed = seed_ + 7919U * generation_ + 104729U * static_cast<uint32_t>(i);
    threads_.emplace_back([this, client_seed] { RunClient(client_seed); });
  }
  ++generation_;
}

void Workload::Stop() {
  stop_.store(true, std::memory_order_release);
  for (std::thread& t : threads_) t.join();
  threads_.clear();
}

std::vector<uint64_t> Workload::acked_ack_keys() const {
  std::lock_guard lock(acked_mu_);
  return acked_keys_;
}

// Ends quietly when the server goes away: a reply that never comes is not acknowledged.
void Workload::RunClient(uint32_t seed) {
  Rng rng(seed);
  const std::unique_ptr<RespClient> c = RespClient::Connect(port_);
  if (c == nullptr) return;
  const bool expiry = mix_ == Mix::kAll;
  while (!stop_.load(std::memory_order_acquire)) {
    const Op op = NextOp(rng, expiry, next_ack_);
    if (!c->SendRaw(Encode(op.commands))) return;
    std::optional<RespValue> last;
    for (size_t i = 0; i < op.commands.size(); ++i) {
      last = c->Read();
      if (!last.has_value()) return;
    }
    if (last->type == RespValue::Type::kError) continue;  // WRONGTYPE, no such key, EXECABORT ...
    acked_writes_.fetch_add(1, std::memory_order_relaxed);
    if (op.ack.has_value() && last->type == RespValue::Type::kSimple && last->str == "OK") {
      std::lock_guard lock(acked_mu_);
      acked_keys_.push_back(*op.ack);
    }
  }
}

}  // namespace tideskv::test
