#include "repl/wait_list.h"

#include <algorithm>
#include <utility>

namespace tideskv::repl {

int64_t CountAtLeast(const std::vector<uint64_t>& acks, uint64_t seq) {
  return static_cast<int64_t>(
      std::count_if(acks.begin(), acks.end(), [seq](uint64_t a) { return a >= seq; }));
}

void WaitList::Add(std::shared_ptr<BlockedClient> client, uint64_t seq, int64_t numreplicas) {
  std::lock_guard lock(mu_);
  entries_.push_back({std::move(client), seq, numreplicas});
}

std::vector<WaitList::Ready> WaitList::Release(const std::vector<uint64_t>& acks) {
  std::vector<Ready> ready;
  std::lock_guard lock(mu_);
  std::vector<Entry> waiting;
  waiting.reserve(entries_.size());
  for (Entry& e : entries_) {
    const int64_t count = CountAtLeast(acks, e.seq);
    if (count >= e.numreplicas) {
      ready.push_back({std::move(e.client), count});
    } else {
      waiting.push_back(std::move(e));
    }
  }
  entries_ = std::move(waiting);
  return ready;
}

// S13 (PF17): ended() takes BlockedClient::mu_, so the clients are collected under mu_ and checked outside
// it. The entries stay in place meanwhile, so a concurrent Release still sees (and may release) them; only
// the ended ones still present are erased afterwards.
void WaitList::Prune() {
  std::vector<std::shared_ptr<BlockedClient>> clients;
  {
    std::lock_guard lock(mu_);
    clients.reserve(entries_.size());
    for (const Entry& e : entries_) clients.push_back(e.client);
  }
  std::erase_if(clients, [](const auto& c) { return !c->ended(); });
  if (clients.empty()) return;
  std::lock_guard lock(mu_);
  std::erase_if(entries_, [&](const Entry& e) {
    return std::find(clients.begin(), clients.end(), e.client) != clients.end();
  });
}

size_t WaitList::size() const {
  std::lock_guard lock(mu_);
  return entries_.size();
}

}  // namespace tideskv::repl
