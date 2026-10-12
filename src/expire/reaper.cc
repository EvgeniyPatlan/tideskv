#include "expire/reaper.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <span>

#include "common/coding.h"
#include "encoding/key_codec.h"
#include "types/object_store.h"

namespace tideskv {

Status ExpireReaper::CollectDue(uint8_t db, int64_t now_ms, std::vector<Due>* out) {
  out->clear();
  return engine_.Read([&](ReadView& view) {
    auto it = view.NewIterator(
        ColumnFamily::kExpire,
        {encoding::DbPrefix(db), encoding::ExpireUpperBound(db, static_cast<uint64_t>(now_ms))});
    if (!it.ok()) return it.status();
    Status st = (*it)->SeekToFirst();
    for (; st.ok() && (*it)->Valid() && out->size() < options_.batch_size; st = (*it)->Next()) {
      encoding::ExpireKey ek;
      if (!encoding::DecodeExpireKey((*it)->key(), &ek) || (*it)->value().size() != 8) {
        return Status::Corruption("bad expire entry");
      }
      out->push_back(
          {std::string(ek.user_key), ek.expire_ms, coding::DecodeFixed64BE((*it)->value().data())});
    }
    return st;
  });
}

Result<size_t> ExpireReaper::DeleteDue(uint8_t db, int64_t now_ms, std::span<const Due> due) {
  std::vector<std::string_view> keys;
  keys.reserve(due.size());
  for (const Due& d : due) keys.push_back(d.key);
  size_t deleted = 0;
  const Status st = engine_.Write(db, keys, [&](WriteTxn& txn) {
    deleted = 0;  // fn re-runs after a commit conflict
    for (const Due& d : due) {
      auto obj = LoadObject(txn, db, d.key, now_ms);
      if (!obj.ok()) return obj.status();
      // kExpired (not just "exists"): a bogus entry can never delete live, non-expiring data.
      const bool same = obj->state == ObjectState::kExpired &&
                        static_cast<uint64_t>(obj->meta.expire_ms) == d.expire_ms && obj->meta.oid == d.oid;
      const Status s = same ? DeleteObject(txn, db, d.key, obj->meta)
                            : DeleteStaleExpireEntry(txn, db, d.key, d.expire_ms);
      if (!s.ok()) return s;
      if (same) ++deleted;
    }
    return Status::Ok();
  });
  if (!st.ok()) return st;
  expired_total_.fetch_add(deleted, std::memory_order_relaxed);  // counted per commit
  return deleted;
}

Result<size_t> ExpireReaper::ReapBatch(uint8_t db, int64_t now_ms, bool* full) {
  std::vector<Due> due;
  if (Status st = CollectDue(db, now_ms, &due); !st.ok()) return st;
  *full = due.size() >= options_.batch_size;
  size_t deleted = 0;
  for (size_t i = 0; i < due.size(); i += kKeysPerWrite) {
    const size_t n = std::min(kKeysPerWrite, due.size() - i);
    auto chunk = DeleteDue(db, now_ms, std::span<const Due>(due).subspan(i, n));
    if (!chunk.ok()) return chunk.status();
    deleted += *chunk;
  }
  return deleted;
}

Result<size_t> ExpireReaper::RunOnce() {
  const auto deadline = std::chrono::steady_clock::now() + options_.cycle_budget;
  const int64_t now = engine_.NowMs();
  size_t total = 0;
  for (uint8_t db = 0; db < encoding::kNumDatabases; ++db) {
    bool full = true;
    while (full) {
      auto n = ReapBatch(db, now, &full);
      if (!n.ok()) return n.status();  // ends the cycle; committed deletes are already counted
      total += *n;
      if (std::chrono::steady_clock::now() >= deadline) full = false;
    }
  }
  return total;
}

void ExpireReaper::Start() {
  std::lock_guard lock(mu_);
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
}

void ExpireReaper::Stop() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void ExpireReaper::Loop() {
  std::unique_lock lock(mu_);
  while (!stop_) {
    lock.unlock();
    try {  // an exception escaping this thread would std::terminate the server
      if (auto n = RunOnce(); !n.ok()) spdlog::error("expire reaper: {}", n.status().ToString());
    } catch (const std::exception& e) {
      spdlog::error("expire reaper: {}", e.what());
    }
    lock.lock();
    cv_.wait_for(lock, options_.interval, [this] { return stop_; });
  }
}

}  // namespace tideskv
