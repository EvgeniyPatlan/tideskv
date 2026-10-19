#include "storage/tides_storage.h"

#include <array>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "storage/tides_status.h"
#include "storage/validation.h"

extern "C" {
#include <db.h>
}

namespace tideskv {
namespace {

using CfHandles = std::array<tidesdb_column_family_t*, kColumnFamilyCount>;

const uint8_t* Bytes(std::string_view s) {
  static const char kEmpty = 0;  // TidesDB wants non-null pointers even for empty values
  return reinterpret_cast<const uint8_t*>(s.empty() ? &kEmpty : s.data());
}

// Retries a call while the engine reports transient contention (spec §5.4).
template <typename F>
int RetryLocked(F&& call) {
  constexpr int kAttempts = 5;
  int rc = call();
  for (int i = 1; i < kAttempts && rc == TDB_ERR_LOCKED; ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(50 << (i - 1)));
    rc = call();
  }
  return rc;
}

// Owns a buffer that TidesDB allocated for us.
struct TdbBuffer {
  uint8_t* data = nullptr;
  size_t size = 0;
  TdbBuffer() = default;
  TdbBuffer(const TdbBuffer&) = delete;
  TdbBuffer& operator=(const TdbBuffer&) = delete;
  ~TdbBuffer() { tidesdb_free(data); }
  std::string_view view() const { return {reinterpret_cast<const char*>(data), size}; }
};

int ToTdbSync(SyncMode m) {
  switch (m) {
    case SyncMode::kNone:
      return TDB_SYNC_NONE;
    case SyncMode::kInterval:
      return TDB_SYNC_INTERVAL;
    case SyncMode::kFull:
      return TDB_SYNC_FULL;
  }
  return TDB_SYNC_INTERVAL;
}

class TidesIterator final : public Iterator {
 public:
  TidesIterator(tidesdb_iter_t* it, IterOptions opts) : it_(it), opts_(std::move(opts)) {}
  ~TidesIterator() override { tidesdb_iter_free(it_); }

  Status SeekToFirst() override {
    if (!opts_.lower.empty()) return Seek(opts_.lower);
    return Position(RetryLocked([&] { return tidesdb_iter_seek_to_first(it_); }), "iter seek_to_first");
  }

  Status SeekToLast() override {
    if (opts_.upper.empty()) {
      return Position(RetryLocked([&] { return tidesdb_iter_seek_to_last(it_); }), "iter seek_to_last");
    }
    Status st = RawSeekForPrev(opts_.upper);
    if (st.ok() && engine_valid_ && key_ == opts_.upper) st = Step(false);  // upper is exclusive
    return st;
  }

  Status Seek(std::string_view target) override {
    if (target.empty()) return SeekToFirst();  // the engine rejects empty keys
    const std::string_view t = (!opts_.lower.empty() && target < opts_.lower) ? opts_.lower : target;
    return Position(RetryLocked([&] { return tidesdb_iter_seek(it_, Bytes(t), t.size()); }), "iter seek");
  }

  Status SeekForPrev(std::string_view target) override {
    if (target.empty()) {  // no key is <= ""; the engine rejects empty keys
      engine_valid_ = false;
      return Status::Ok();
    }
    if (!opts_.upper.empty() && target >= opts_.upper) return SeekToLast();
    return RawSeekForPrev(target);
  }

  Status Next() override { return Valid() ? Step(true) : Status::InvalidArgument("iterator not valid"); }
  Status Prev() override { return Valid() ? Step(false) : Status::InvalidArgument("iterator not valid"); }

  bool Valid() const override {
    return engine_valid_ && (opts_.lower.empty() || key_ >= opts_.lower) &&
           (opts_.upper.empty() || key_ < opts_.upper);
  }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }

 private:
  Status RawSeekForPrev(std::string_view t) {
    return Position(RetryLocked([&] { return tidesdb_iter_seek_for_prev(it_, Bytes(t), t.size()); }),
                    "iter seek_for_prev");
  }

  Status Step(bool forward) {
    return Position(RetryLocked([&] { return forward ? tidesdb_iter_next(it_) : tidesdb_iter_prev(it_); }),
                    forward ? "iter next" : "iter prev");
  }

  // Loads the engine's current entry. End of data is OK with engine_valid_ = false.
  Status Position(int rc, const char* what) {
    engine_valid_ = false;
    if (rc == TDB_ERR_NOT_FOUND) return Status::Ok();
    if (rc != TDB_SUCCESS) return FromTdb(rc, what);
    if (!tidesdb_iter_valid(it_)) return Status::Ok();
    TdbBuffer k, v;
    rc = tidesdb_iter_key_value(it_, &k.data, &k.size, &v.data, &v.size);
    if (rc != TDB_SUCCESS) return FromTdb(rc, "iter key_value");
    key_.assign(k.view());
    value_.assign(v.view());
    engine_valid_ = true;
    return Status::Ok();
  }

  tidesdb_iter_t* it_;
  IterOptions opts_;
  std::string key_;
  std::string value_;
  bool engine_valid_ = false;
};

Result<std::string> TxnGet(tidesdb_txn_t* txn, tidesdb_column_family_t* cf, std::string_view key) {
  if (Status st = ValidateKey(key); !st.ok()) return st;
  TdbBuffer v;
  const int rc = RetryLocked([&] {
    tidesdb_free(v.data);  // never leak a buffer from an earlier attempt
    v.data = nullptr;
    v.size = 0;
    return tidesdb_txn_get(txn, cf, Bytes(key), key.size(), &v.data, &v.size);
  });
  if (rc != TDB_SUCCESS) return FromTdb(rc, "get");
  return std::string(v.view());
}

// Always an unbounded engine iterator: TidesIterator enforces IterOptions itself. The engine's
// bounded tidesdb_iter_new_range prunes sstables by their point-key min/max, which ignores the
// extent of a range tombstone, so a flushed DeleteRange whose table holds no key inside the range
// is skipped and the older data it deleted reappears (see tests/stress).
Result<std::unique_ptr<Iterator>> TxnIterator(tidesdb_txn_t* txn, tidesdb_column_family_t* cf,
                                              IterOptions opts) {
  tidesdb_iter_t* it = nullptr;
  const int rc = RetryLocked([&] { return tidesdb_iter_new(txn, cf, &it); });
  if (rc != TDB_SUCCESS) return FromTdb(rc, "iter_new");
  return std::unique_ptr<Iterator>(std::make_unique<TidesIterator>(it, std::move(opts)));
}

// Same semantics as encoding::PrefixUpperBound (smallest string above every string with this prefix;
// empty = no bound). Kept local: the storage layer does not depend on the encoding layer.
std::string PrefixUpperBoundLocal(std::string_view prefix) {
  std::string out(prefix);
  while (!out.empty() && static_cast<uint8_t>(out.back()) == 0xff) out.pop_back();
  if (!out.empty()) out.back() = static_cast<char>(static_cast<uint8_t>(out.back()) + 1);
  return out;
}

class TidesReadView final : public ReadView {
 public:
  TidesReadView(tidesdb_txn_t* txn, tidesdb_snapshot_t* snapshot, CfHandles cfs)
      : txn_(txn), snapshot_(snapshot), cfs_(cfs) {}
  ~TidesReadView() override {
    tidesdb_txn_free(txn_);               // first: it reads versions the snapshot keeps alive
    tidesdb_snapshot_release(snapshot_);  // NULL is allowed
  }

  Result<std::string> Get(ColumnFamily cf, std::string_view key) override {
    return TxnGet(txn_, cfs_[CfIndex(cf)], key);
  }
  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts) override {
    return TxnIterator(txn_, cfs_[CfIndex(cf)], std::move(opts));
  }

 private:
  tidesdb_txn_t* txn_;
  tidesdb_snapshot_t* snapshot_;
  CfHandles cfs_;
};

class TidesWriteTxn final : public WriteTxn {
 public:
  TidesWriteTxn(tidesdb_txn_t* txn, CfHandles cfs, std::mutex& commit_mu)
      : txn_(txn), cfs_(cfs), commit_mu_(commit_mu) {}
  ~TidesWriteTxn() override { tidesdb_txn_free(txn_); }  // rolls back if still open

  Result<std::string> Get(ColumnFamily cf, std::string_view key) override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    return TxnGet(txn_, Cf(cf), key);
  }
  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts) override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    return TxnIterator(txn_, Cf(cf), std::move(opts));
  }

  Status Put(ColumnFamily cf, std::string_view key, std::string_view value) override {
    if (Status st = Check(ValidateKey(key)); !st.ok()) return st;
    return FromTdb(tidesdb_txn_put(txn_, Cf(cf), Bytes(key), key.size(), Bytes(value), value.size(), 0),
                   "put");
  }
  Status Delete(ColumnFamily cf, std::string_view key) override {
    if (Status st = Check(ValidateKey(key)); !st.ok()) return st;
    return FromTdb(tidesdb_txn_delete(txn_, Cf(cf), Bytes(key), key.size()), "delete");
  }
  // D2 (M6a): TidesDB 10.1.1 holds at most 32 distinct range intervals per commit (global table,
  // TDB_MVCC_MAX_RANGE_RESERVATIONS). Past kMaxNativeRangeDeletes distinct ranges, further ones are
  // applied as point deletes of the keys currently in range (this txn's own buffered writes
  // included): the same result at O(keys in range).
  Status DeletePrefix(ColumnFamily cf, std::string_view prefix) override {
    if (Status st = Check(ValidatePrefix(prefix)); !st.ok()) return st;
    // Tagged "\x01" + prefix so the triple never equals a DeleteRange triple.
    if (UseNativeRange(cf, std::string(prefix), "\x01" + std::string(prefix))) {
      return FromTdb(tidesdb_txn_delete_prefix(txn_, Cf(cf), Bytes(prefix), prefix.size()), "delete_prefix");
    }
    return DeleteKeysIn(cf, std::string(prefix), PrefixUpperBoundLocal(prefix));
  }
  Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
    if (Status st = Check(ValidateRange(lo, hi)); !st.ok()) return st;
    if (UseNativeRange(cf, std::string(lo), std::string(hi))) {
      const uint8_t* hi_ptr = hi.empty() ? nullptr : Bytes(hi);
      return FromTdb(tidesdb_txn_delete_range(txn_, Cf(cf), Bytes(lo), lo.size(), hi_ptr, hi.size()),
                     "delete_range");
    }
    return DeleteKeysIn(cf, std::string(lo), std::string(hi));
  }

  Status Savepoint(std::string_view name) override {
    if (Status st = Check(ValidateSavepointName(name)); !st.ok()) return st;
    return FromTdb(tidesdb_txn_savepoint(txn_, std::string(name).c_str()), "savepoint");
  }
  Status RollbackToSavepoint(std::string_view name) override {
    if (Status st = Check(ValidateSavepointName(name)); !st.ok()) return st;
    const int rc = tidesdb_txn_rollback_to_savepoint(txn_, std::string(name).c_str());
    if (rc == TDB_ERR_NOT_FOUND) return Status::NotFound("no savepoint named " + std::string(name));
    return FromTdb(rc, "rollback_to_savepoint");
  }
  Status ReleaseSavepoint(std::string_view name) override {
    if (Status st = Check(ValidateSavepointName(name)); !st.ok()) return st;
    const int rc = tidesdb_txn_release_savepoint(txn_, std::string(name).c_str());
    if (rc == TDB_ERR_NOT_FOUND) return Status::NotFound("no savepoint named " + std::string(name));
    return FromTdb(rc, "release_savepoint");
  }

  Status Commit() override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    finished_ = true;                        // a failed commit is final too; callers retry the whole command
    const std::lock_guard lock(commit_mu_);  // D1: see TidesStorage::Impl::commit_mu
    return FromTdb(tidesdb_txn_commit(txn_), "commit");
  }
  Status Rollback() override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    finished_ = true;
    return FromTdb(tidesdb_txn_rollback(txn_), "rollback");
  }

 private:
  tidesdb_column_family_t* Cf(ColumnFamily cf) const { return cfs_[CfIndex(cf)]; }
  Status Check(Status arg_status) const {
    if (finished_) return Status::InvalidArgument("transaction already finished");
    return arg_status;
  }

  // True when this range may be sent to the engine as a native range delete: it is already reserved
  // (the engine de-duplicates identical intervals) or a slot below the cap is free, which is then
  // taken. A rolled-back savepoint keeps its slots, which only makes the cap conservative.
  bool UseNativeRange(ColumnFamily cf, std::string lo, std::string hi) {
    if (native_ranges_.size() >= kMaxNativeRangeDeletes && !native_ranges_.contains({cf, lo, hi})) {
      return false;
    }
    native_ranges_.emplace(cf, std::move(lo), std::move(hi));
    return true;
  }

  // Point-deletes every key in [lo, hi) (empty hi = to the end). Keys are collected first: never
  // delete while iterating.
  Status DeleteKeysIn(ColumnFamily cf, std::string lo, std::string hi) {
    std::vector<std::string> keys;
    {
      auto created = TxnIterator(txn_, Cf(cf), {std::move(lo), std::move(hi)});
      if (!created.ok()) return created.status();
      std::unique_ptr<Iterator> it = std::move(created).value();
      if (Status st = it->SeekToFirst(); !st.ok()) return st;
      while (it->Valid()) {
        keys.emplace_back(it->key());
        if (Status st = it->Next(); !st.ok()) return st;
      }
    }
    for (const std::string& key : keys) {
      if (Status st = FromTdb(tidesdb_txn_delete(txn_, Cf(cf), Bytes(key), key.size()), "delete"); !st.ok()) {
        return st;
      }
    }
    return Status::Ok();
  }

  tidesdb_txn_t* txn_;
  CfHandles cfs_;
  std::mutex& commit_mu_;
  std::set<std::tuple<ColumnFamily, std::string, std::string>> native_ranges_;
  bool finished_ = false;
};

// The compression algorithm actually usable for `wanted` in this build.
Compression Effective(Compression wanted) {
  if (wanted == Compression::kNone) return Compression::kNone;
  const auto algo = wanted == Compression::kZstd ? TDB_COMPRESS_ZSTD : TDB_COMPRESS_LZ4;
  if (tidesdb_compression_available(algo) != 0) return wanted;
  spdlog::warn("compression '{}' is not available in this build; storing data uncompressed",
               CompressionName(wanted));
  return Compression::kNone;
}

void SetPipeline(tidesdb_column_family_config_t* cfg, Compression c) {
  cfg->encoding_count = 0;
  if (c == Compression::kNone) return;
  cfg->encoding_pipeline[0] =
      static_cast<uint8_t>(c == Compression::kZstd ? TDB_COMPRESS_ZSTD : TDB_COMPRESS_LZ4);
  cfg->encoding_count = 1;
}

// The per-family settings TidesKV chooses; everything else stays as created or persisted.
struct FamilySettings {
  Compression compression = Compression::kNone;
  int l1_file_count_trigger = 0;  // 0 = keep (default at creation)
};

// TidesDB 10.1.1's default (1) puts the dividing level on the flush tier of a 3-level family, and then
// a level over capacity is rewritten into itself forever, compacting with no writes at all. At 0 the
// dividing level is L-1, so an over-capacity L-1 takes the partitioned merge into the largest level.
constexpr int kDividingLevelOffset = 0;

void ApplySettings(tidesdb_column_family_config_t* cfg, const FamilySettings& f) {
  SetPipeline(cfg, f.compression);
  if (f.l1_file_count_trigger > 0) cfg->l1_file_count_trigger = f.l1_file_count_trigger;
  cfg->dividing_level_offset = kDividingLevelOffset;
}

// Existing families keep the config persisted in the manifest unless we change it.
// The update contends with background compaction (TDB_ERR_LOCKED); if it never wins, Open must
// not fail, so we keep the persisted config (new data is still readable either way).
Status ApplyFamilySettings(tidesdb_t* db, tidesdb_column_family_t* cf, const FamilySettings& f) {
  tidesdb_cf_stats_t st{};
  if (int rc = tidesdb_get_cf_stats(cf, &st); rc != TDB_SUCCESS) return FromTdb(rc, "get_cf_stats");
  tidesdb_column_family_config_t cfg = st.config;
  ApplySettings(&cfg, f);
  const bool same = cfg.encoding_count == st.config.encoding_count &&
                    (cfg.encoding_count == 0 || cfg.encoding_pipeline[0] == st.config.encoding_pipeline[0]) &&
                    cfg.l1_file_count_trigger == st.config.l1_file_count_trigger &&
                    cfg.dividing_level_offset == st.config.dividing_level_offset;
  if (same) return Status::Ok();
  constexpr int kAttempts = 10;
  int rc = TDB_ERR_LOCKED;
  for (int i = 0; i < kAttempts && rc == TDB_ERR_LOCKED; ++i) {
    if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rc = tidesdb_cf_update_runtime_config(db, cf, &cfg, /*persist_to_disk=*/1);
  }
  if (rc == TDB_ERR_LOCKED) {
    spdlog::warn("column family is busy compacting; keeping its persisted settings");
    return Status::Ok();
  }
  return FromTdb(rc, "cf_update_runtime_config");
}

Status EnsureColumnFamily(tidesdb_t* db, ColumnFamily cf, const FamilySettings& f,
                          tidesdb_column_family_t** out) {
  const std::string name(ColumnFamilyName(cf));
  *out = tidesdb_get_column_family(db, name.c_str());
  if (*out != nullptr) return ApplyFamilySettings(db, *out, f);
  tidesdb_column_family_config_t cfg = tidesdb_default_column_family_config();
  ApplySettings(&cfg, f);
  const int rc = tidesdb_create_column_family(db, name.c_str(), &cfg);
  if (rc != TDB_SUCCESS && rc != TDB_ERR_EXISTS) return FromTdb(rc, "create column family " + name);
  *out = tidesdb_get_column_family(db, name.c_str());
  return *out != nullptr ? Status::Ok() : Status::Internal("column family missing after create: " + name);
}

}  // namespace

struct TidesStorage::Impl {
  tidesdb_t* db = nullptr;
  CfHandles cfs{};
  Compression compression = Compression::kNone;
  // D1 (M6a): TidesDB 10.1.1 can tear or lose concurrently committed txns across a memtable
  // rotation on kill -9 (notes/m6-research/tidesdb-replication.md §5.2). Every commit and
  // checkpoint (a forced rotation) runs under this mutex until upstream fixes the split-apply race.
  std::mutex commit_mu;
  ~Impl() {
    if (db != nullptr) tidesdb_close(db);
  }
};

TidesStorage::TidesStorage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TidesStorage::~TidesStorage() = default;

Result<std::unique_ptr<TidesStorage>> TidesStorage::Open(const StorageOptions& o) {
  auto impl = std::make_unique<Impl>();
  tidesdb_config_t cfg = tidesdb_default_config();
  std::string path = o.path;  // TidesDB copies it during open
  cfg.db_path = path.data();
  cfg.log_level = TDB_LOG_WARN;
  cfg.memtable_sync_mode = ToTdbSync(o.sync_mode);
  cfg.memtable_sync_interval_us = o.sync_interval_us;
  if (o.block_cache_size != 0) cfg.block_cache_size = o.block_cache_size;
  if (o.memtable_size != 0) cfg.memtable_write_buffer_size = o.memtable_size;
  if (o.flush_threads != 0) cfg.num_flush_threads = o.flush_threads;
  if (o.compaction_threads != 0) cfg.num_compaction_threads = o.compaction_threads;
  if (o.l0_stall_threshold != 0) cfg.memtable_l0_queue_stall_threshold = o.l0_stall_threshold;

  const int rc = tidesdb_open(&cfg, &impl->db);
  if (rc == TDB_ERR_LOCKED) {
    return Status::Busy("open " + o.path + ": data directory is locked by another process or handle");
  }
  if (rc != TDB_SUCCESS) return FromTdb(rc, "open " + o.path);
  const Compression comp = Effective(o.compression);
  const FamilySettings settings{comp, o.l1_file_count_trigger};
  for (ColumnFamily cf : kAllColumnFamilies) {
    if (Status st = EnsureColumnFamily(impl->db, cf, settings, &impl->cfs[CfIndex(cf)]); !st.ok()) return st;
  }
  impl->compression = comp;
  return std::unique_ptr<TidesStorage>(new TidesStorage(std::move(impl)));
}

Result<std::unique_ptr<WriteTxn>> TidesStorage::BeginWrite() {
  tidesdb_txn_t* txn = nullptr;
  const int rc = tidesdb_txn_begin_with_isolation(impl_->db, TDB_ISOLATION_READ_COMMITTED, &txn);
  if (rc != TDB_SUCCESS) return FromTdb(rc, "txn_begin");
  return std::unique_ptr<WriteTxn>(std::make_unique<TidesWriteTxn>(txn, impl_->cfs, impl_->commit_mu));
}

Result<std::unique_ptr<ReadView>> TidesStorage::BeginRead() {
  tidesdb_txn_t* txn = nullptr;
  const int rc = tidesdb_txn_begin_with_isolation(impl_->db, TDB_ISOLATION_READ_COMMITTED, &txn);
  if (rc != TDB_SUCCESS) return FromTdb(rc, "txn_begin");
  return std::unique_ptr<ReadView>(std::make_unique<TidesReadView>(txn, nullptr, impl_->cfs));
}

Result<std::unique_ptr<ReadView>> TidesStorage::BeginSnapshot() {
  tidesdb_snapshot_t* snapshot = nullptr;
  int rc = tidesdb_snapshot_create(impl_->db, &snapshot);
  if (rc != TDB_SUCCESS) return FromTdb(rc, "snapshot_create");
  tidesdb_txn_t* txn = nullptr;
  rc = tidesdb_txn_begin_at_snapshot(impl_->db, snapshot, &txn);
  if (rc != TDB_SUCCESS) {
    tidesdb_snapshot_release(snapshot);
    return FromTdb(rc, "txn_begin_at_snapshot");
  }
  return std::unique_ptr<ReadView>(std::make_unique<TidesReadView>(txn, snapshot, impl_->cfs));
}

Status TidesStorage::Backup(const std::string& dir) {
  return FromTdb(tidesdb_backup(impl_->db, dir.c_str()), "backup");
}

Status TidesStorage::Checkpoint() {
  const std::lock_guard lock(impl_->commit_mu);  // D1: a forced rotation must not split a commit
  return FromTdb(tidesdb_checkpoint(impl_->db), "checkpoint");
}

std::string_view CompressionName(Compression c) {
  switch (c) {
    case Compression::kNone:
      return "no";
    case Compression::kLz4:
      return "lz4";
    case Compression::kZstd:
      return "zstd";
  }
  return "no";
}

Compression TidesStorage::compression() const { return impl_->compression; }

// Whole-CF only. tidesdb_compact_range (v10.1.1) picks its inputs by each sstable's point-key
// min/max. The flush of a memtable that held only a range delete (the usual FLUSHDB) writes an
// sstable with the tombstone and no keys, whose empty min/max never meets a range starting at a
// db prefix, so the merge runs without the tombstone and keeps every deleted key.
Status TidesStorage::Compact(ColumnFamily cf) {
  return FromTdb(tidesdb_compact(impl_->db, impl_->cfs[CfIndex(cf)]), "compact");
}

Result<StorageStats> TidesStorage::Stats() {
  StorageStats out;
  tidesdb_db_stats_t db{};
  if (int rc = tidesdb_get_db_stats(impl_->db, &db); rc != TDB_SUCCESS) return FromTdb(rc, "get_db_stats");
  out.flush_count = db.flush_count;
  out.compaction_count = db.compaction_count;
  out.immutable_memtables = db.immutable_memtable_count;
  for (ColumnFamily cf : kAllColumnFamilies) {
    tidesdb_cf_stats_t st{};
    if (int rc = tidesdb_get_cf_stats(impl_->cfs[CfIndex(cf)], &st); rc != TDB_SUCCESS) {
      return FromTdb(rc, "get_cf_stats");
    }
    out.cf[CfIndex(cf)] = {st.total_keys, st.total_tombstones, st.compaction_count,
                           st.config.l1_file_count_trigger, st.config.dividing_level_offset};
  }
  return out;
}

}  // namespace tideskv
