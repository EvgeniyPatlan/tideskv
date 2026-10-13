#include "storage/memory_storage.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include "storage/validation.h"

namespace tideskv {
namespace {

using Table = InMemoryStorage::Table;
using Tables = InMemoryStorage::Tables;
using Op = InMemoryStorage::Op;
using Rows = std::vector<std::pair<std::string, std::string>>;

bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool OpCovers(const Op& op, std::string_view key) {
  switch (op.kind) {
    case Op::Kind::kPut:
    case Op::Kind::kDelete:
      return op.a == key;
    case Op::Kind::kDeletePrefix:
      return StartsWith(key, op.a);
    case Op::Kind::kDeleteRange:
      return key >= op.a && (op.b.empty() || key < op.b);
  }
  return false;
}

void ApplyOp(Table& t, const Op& op) {
  switch (op.kind) {
    case Op::Kind::kPut:
      t.insert_or_assign(op.a, op.b);
      return;
    case Op::Kind::kDelete:
      t.erase(op.a);
      return;
    case Op::Kind::kDeletePrefix: {
      auto it = t.lower_bound(op.a);
      while (it != t.end() && StartsWith(it->first, op.a)) it = t.erase(it);
      return;
    }
    case Op::Kind::kDeleteRange:
      t.erase(t.lower_bound(op.a), op.b.empty() ? t.end() : t.lower_bound(op.b));
      return;
  }
}

// Iterates a materialised, already-bounded, sorted vector of rows.
class VectorIterator final : public Iterator {
 public:
  explicit VectorIterator(Rows rows) : rows_(std::move(rows)), pos_(rows_.size()) {}

  Status SeekToFirst() override {
    pos_ = 0;
    return Status::Ok();
  }
  Status SeekToLast() override {
    pos_ = rows_.empty() ? 0 : rows_.size() - 1;
    return Status::Ok();
  }
  Status Seek(std::string_view target) override {
    pos_ = LowerBound(target);
    return Status::Ok();
  }
  Status SeekForPrev(std::string_view target) override {
    const size_t ub = UpperBound(target);
    pos_ = ub == 0 ? rows_.size() : ub - 1;
    return Status::Ok();
  }
  Status Next() override {
    if (!Valid()) return Status::InvalidArgument("iterator not valid");
    ++pos_;
    return Status::Ok();
  }
  Status Prev() override {
    if (!Valid()) return Status::InvalidArgument("iterator not valid");
    pos_ = pos_ == 0 ? rows_.size() : pos_ - 1;
    return Status::Ok();
  }
  bool Valid() const override { return pos_ < rows_.size(); }
  std::string_view key() const override { return rows_[pos_].first; }
  std::string_view value() const override { return rows_[pos_].second; }

 private:
  size_t LowerBound(std::string_view t) const {
    return static_cast<size_t>(
        std::lower_bound(rows_.begin(), rows_.end(), t,
                         [](const auto& r, std::string_view v) { return r.first < v; }) -
        rows_.begin());
  }
  size_t UpperBound(std::string_view t) const {
    return static_cast<size_t>(
        std::upper_bound(rows_.begin(), rows_.end(), t,
                         [](std::string_view v, const auto& r) { return v < r.first; }) -
        rows_.begin());
  }

  Rows rows_;
  size_t pos_;
};

std::unique_ptr<Iterator> MakeIterator(const Table& t, const IterOptions& o) {
  Rows rows;
  auto it = o.lower.empty() ? t.begin() : t.lower_bound(o.lower);
  for (; it != t.end() && (o.upper.empty() || it->first < o.upper); ++it)
    rows.emplace_back(it->first, it->second);
  return std::make_unique<VectorIterator>(std::move(rows));
}

class MemoryReadView final : public ReadView {
 public:
  // frozen == nullptr: read-committed (every call sees the latest commit).
  MemoryReadView(const InMemoryStorage& s, std::shared_ptr<const Tables> frozen)
      : storage_(s), frozen_(std::move(frozen)) {}

  Result<std::string> Get(ColumnFamily cf, std::string_view key) override {
    if (Status st = ValidateKey(key); !st.ok()) return st;
    const auto tables = View();
    const Table& t = (*tables)[CfIndex(cf)];
    const auto it = t.find(key);
    if (it == t.end()) return Status::NotFound();
    return it->second;
  }

  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts) override {
    const auto tables = View();
    return MakeIterator((*tables)[CfIndex(cf)], opts);
  }

 private:
  std::shared_ptr<const Tables> View() const { return frozen_ ? frozen_ : storage_.Current(); }

  const InMemoryStorage& storage_;
  std::shared_ptr<const Tables> frozen_;
};

class MemoryWriteTxn final : public WriteTxn {
 public:
  explicit MemoryWriteTxn(InMemoryStorage& s) : storage_(s) {}

  Result<std::string> Get(ColumnFamily cf, std::string_view key) override {
    if (Status st = Check(ValidateKey(key)); !st.ok()) return st;
    const auto tables = storage_.Current();
    const Table& t = (*tables)[CfIndex(cf)];
    std::optional<std::string> value;
    if (const auto it = t.find(key); it != t.end()) value = it->second;
    for (const Op& op : ops_) {
      if (op.cf != cf || !OpCovers(op, key)) continue;
      if (op.kind == Op::Kind::kPut)
        value = op.b;
      else
        value.reset();
    }
    if (!value) return Status::NotFound();
    return std::move(*value);
  }

  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts) override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    Table merged = (*storage_.Current())[CfIndex(cf)];
    for (const Op& op : ops_) {
      if (op.cf == cf) ApplyOp(merged, op);
    }
    return MakeIterator(merged, opts);
  }

  Status Put(ColumnFamily cf, std::string_view key, std::string_view value) override {
    return Buffer(ValidateKey(key), Op{Op::Kind::kPut, cf, std::string(key), std::string(value)});
  }
  Status Delete(ColumnFamily cf, std::string_view key) override {
    return Buffer(ValidateKey(key), Op{Op::Kind::kDelete, cf, std::string(key), {}});
  }
  Status DeletePrefix(ColumnFamily cf, std::string_view prefix) override {
    return Buffer(ValidatePrefix(prefix), Op{Op::Kind::kDeletePrefix, cf, std::string(prefix), {}});
  }
  Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
    return Buffer(ValidateRange(lo, hi), Op{Op::Kind::kDeleteRange, cf, std::string(lo), std::string(hi)});
  }

  Status Savepoint(std::string_view name) override {
    if (Status st = Check(ValidateSavepointName(name)); !st.ok()) return st;
    savepoints_.emplace_back(std::string(name), ops_.size());
    return Status::Ok();
  }

  Status RollbackToSavepoint(std::string_view name) override {
    if (Status st = Check(ValidateSavepointName(name)); !st.ok()) return st;
    for (size_t i = savepoints_.size(); i-- > 0;) {
      if (savepoints_[i].first != name) continue;
      ops_.resize(savepoints_[i].second);
      savepoints_.resize(i + 1);  // keep this savepoint, drop later ones
      return Status::Ok();
    }
    return Status::NotFound("no savepoint named " + std::string(name));
  }

  Status ReleaseSavepoint(std::string_view name) override {
    if (Status st = Check(ValidateSavepointName(name)); !st.ok()) return st;
    for (size_t i = savepoints_.size(); i-- > 0;) {
      if (savepoints_[i].first != name) continue;
      savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(i));  // the ops stay
      return Status::Ok();
    }
    return Status::NotFound("no savepoint named " + std::string(name));
  }

  Status Commit() override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    storage_.Apply(ops_);
    finished_ = true;
    return Status::Ok();
  }

  Status Rollback() override {
    if (Status st = Check(Status::Ok()); !st.ok()) return st;
    ops_.clear();
    finished_ = true;
    return Status::Ok();
  }

 private:
  // Finished-state errors take precedence over argument errors.
  Status Check(Status arg_status) const {
    if (finished_) return Status::InvalidArgument("transaction already finished");
    return arg_status;
  }

  Status Buffer(Status arg_status, Op op) {
    if (Status st = Check(std::move(arg_status)); !st.ok()) return st;
    ops_.push_back(std::move(op));
    return Status::Ok();
  }

  InMemoryStorage& storage_;
  std::vector<Op> ops_;
  std::vector<std::pair<std::string, size_t>> savepoints_;
  bool finished_ = false;
};

}  // namespace

InMemoryStorage::InMemoryStorage() : tables_(std::make_shared<Tables>()) {}

std::shared_ptr<const InMemoryStorage::Tables> InMemoryStorage::Current() const {
  std::lock_guard lock(mu_);
  return tables_;
}

void InMemoryStorage::Apply(const std::vector<Op>& ops) {
  std::lock_guard lock(mu_);
  auto next = std::make_shared<Tables>(*tables_);
  for (const Op& op : ops) ApplyOp((*next)[CfIndex(op.cf)], op);
  tables_ = std::move(next);
}

Result<std::unique_ptr<WriteTxn>> InMemoryStorage::BeginWrite() {
  return std::unique_ptr<WriteTxn>(std::make_unique<MemoryWriteTxn>(*this));
}

Result<std::unique_ptr<ReadView>> InMemoryStorage::BeginRead() {
  return std::unique_ptr<ReadView>(std::make_unique<MemoryReadView>(*this, nullptr));
}

Result<std::unique_ptr<ReadView>> InMemoryStorage::BeginSnapshot() {
  return std::unique_ptr<ReadView>(std::make_unique<MemoryReadView>(*this, Current()));
}

}  // namespace tideskv
