#include "storage/tides_storage.h"

#include <array>
#include <chrono>
#include <thread>
#include <utility>

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

Result<std::unique_ptr<Iterator>> TxnIterator(tidesdb_txn_t* txn, tidesdb_column_family_t* cf,
                                              IterOptions opts) {
  tidesdb_iter_t* it = nullptr;
  int rc = 0;
  if (opts.upper.empty()) {
    rc = RetryLocked([&] { return tidesdb_iter_new(txn, cf, &it); });
  } else {
    // A single NUL byte sorts at or below every possible (non-empty) key.
    const std::string lower = opts.lower.empty() ? std::string(1, '\0') : opts.lower;
    rc = RetryLocked([&] {
      return tidesdb_iter_new_range(txn, cf, Bytes(lower), lower.size(), Bytes(opts.upper), opts.upper.size(),
                                    &it);
    });
  }
  if (rc != TDB_SUCCESS) return FromTdb(rc, "iter_new");
  return std::unique_ptr<Iterator>(std::make_unique<TidesIterator>(it, std::move(opts)));
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
  TidesWriteTxn(tidesdb_txn_t* txn, CfHandles cfs) : txn_(txn), cfs_(cfs) {}
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
  Status DeletePrefix(ColumnFamily cf, std::string_view prefix) override {
    if (Status st = Check(ValidatePrefix(prefix)); !st.ok()) return st;
    return FromTdb(tidesdb_txn_delete_prefix(txn_, Cf(cf), Bytes(prefix), prefix.size()), "delete_prefix");
  }
  Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
    if (Status st = Check(ValidateRange(lo, hi)); !st.ok()) return st;
    const uint8_t* hi_ptr = hi.empty() ? nullptr : Bytes(hi);
    return FromTdb(tidesdb_txn_delete_range(txn_, Cf(cf), Bytes(lo), lo.size(), hi_ptr, hi.size()),
                   "delete_range");
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

  Status Commit() override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    finished_ = true;  // a failed commit is final too; callers retry the whole command
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

  tidesdb_txn_t* txn_;
  CfHandles cfs_;
  bool finished_ = false;
};

Status EnsureColumnFamily(tidesdb_t* db, ColumnFamily cf, tidesdb_column_family_t** out) {
  const std::string name(ColumnFamilyName(cf));
  *out = tidesdb_get_column_family(db, name.c_str());
  if (*out != nullptr) return Status::Ok();
  const tidesdb_column_family_config_t cfg = tidesdb_default_column_family_config();
  const int rc = tidesdb_create_column_family(db, name.c_str(), &cfg);
  if (rc != TDB_SUCCESS && rc != TDB_ERR_EXISTS) return FromTdb(rc, "create column family " + name);
  *out = tidesdb_get_column_family(db, name.c_str());
  return *out != nullptr ? Status::Ok() : Status::Internal("column family missing after create: " + name);
}

}  // namespace

struct TidesStorage::Impl {
  tidesdb_t* db = nullptr;
  CfHandles cfs{};
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

  const int rc = tidesdb_open(&cfg, &impl->db);
  if (rc == TDB_ERR_LOCKED) {
    return Status::Busy("open " + o.path + ": data directory is locked by another process or handle");
  }
  if (rc != TDB_SUCCESS) return FromTdb(rc, "open " + o.path);
  for (ColumnFamily cf : kAllColumnFamilies) {
    if (Status st = EnsureColumnFamily(impl->db, cf, &impl->cfs[CfIndex(cf)]); !st.ok()) return st;
  }
  return std::unique_ptr<TidesStorage>(new TidesStorage(std::move(impl)));
}

Result<std::unique_ptr<WriteTxn>> TidesStorage::BeginWrite() {
  tidesdb_txn_t* txn = nullptr;
  const int rc = tidesdb_txn_begin_with_isolation(impl_->db, TDB_ISOLATION_READ_COMMITTED, &txn);
  if (rc != TDB_SUCCESS) return FromTdb(rc, "txn_begin");
  return std::unique_ptr<WriteTxn>(std::make_unique<TidesWriteTxn>(txn, impl_->cfs));
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

Status TidesStorage::Checkpoint() { return FromTdb(tidesdb_checkpoint(impl_->db), "checkpoint"); }

}  // namespace tideskv
