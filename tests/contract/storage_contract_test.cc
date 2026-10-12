#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "contract/storage_contract.h"

namespace tideskv::test {
namespace {

using Rows = std::vector<std::pair<std::string, std::string>>;
constexpr ColumnFamily kMeta = ColumnFamily::kMeta;
constexpr ColumnFamily kSub = ColumnFamily::kSubkey;

void MustPut(Storage& s, ColumnFamily cf, std::string_view k, std::string_view v) {
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok()) << txn.status().ToString();
  ASSERT_TRUE((*txn)->Put(cf, k, v).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

std::optional<std::string> ReadCommitted(Storage& s, ColumnFamily cf, std::string_view k) {
  auto view = s.BeginRead();
  EXPECT_TRUE(view.ok()) << view.status().ToString();
  if (!view.ok()) return std::nullopt;
  auto r = (*view)->Get(cf, k);
  if (!r.ok()) {
    EXPECT_TRUE(r.status().IsNotFound()) << r.status().ToString();
    return std::nullopt;
  }
  return *r;
}

Rows Scan(ReadView& view, ColumnFamily cf, IterOptions opts = {}) {
  Rows rows;
  auto created = view.NewIterator(cf, std::move(opts));
  EXPECT_TRUE(created.ok()) << created.status().ToString();
  if (!created.ok()) return rows;
  std::unique_ptr<Iterator> it = std::move(created).value();
  EXPECT_TRUE(it->SeekToFirst().ok());
  while (it->Valid()) {
    rows.emplace_back(it->key(), it->value());
    EXPECT_TRUE(it->Next().ok());
  }
  return rows;
}

Rows ScanCommitted(Storage& s, ColumnFamily cf, IterOptions opts = {}) {
  auto view = s.BeginRead();
  EXPECT_TRUE(view.ok());
  return view.ok() ? Scan(**view, cf, std::move(opts)) : Rows{};
}

void PutAll(Storage& s, ColumnFamily cf, const Rows& rows) {
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  for (const auto& [k, v] : rows) ASSERT_TRUE((*txn)->Put(cf, k, v).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

const Rows kAtoE = {{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"}};

TEST_P(StorageContractTest, PutIsVisibleAfterCommit) {
  MustPut(storage(), kMeta, "k", "v");
  EXPECT_EQ(ReadCommitted(storage(), kMeta, "k"), "v");
}

TEST_P(StorageContractTest, MissingKeyIsNotFound) {
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  EXPECT_TRUE((*view)->Get(kMeta, "nope").status().IsNotFound());
}

TEST_P(StorageContractTest, UncommittedWritesAreInvisibleToOthers) {
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Put(kMeta, "k", "v").ok());
  EXPECT_FALSE(ReadCommitted(storage(), kMeta, "k").has_value());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(ReadCommitted(storage(), kMeta, "k"), "v");
}

TEST_P(StorageContractTest, TxnReadsItsOwnWrites) {
  PutAll(storage(), kMeta, {{"a", "1"}, {"c", "3"}});
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Put(kMeta, "b", "2").ok());
  ASSERT_TRUE((*txn)->Delete(kMeta, "c").ok());
  auto b = (*txn)->Get(kMeta, "b");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(*b, "2");
  EXPECT_TRUE((*txn)->Get(kMeta, "c").status().IsNotFound());
  EXPECT_EQ(Scan(**txn, kMeta), (Rows{{"a", "1"}, {"b", "2"}}));
}

TEST_P(StorageContractTest, RollbackDiscardsWrites) {
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Put(kMeta, "k", "v").ok());
  ASSERT_TRUE((*txn)->Rollback().ok());
  EXPECT_FALSE(ReadCommitted(storage(), kMeta, "k").has_value());
}

TEST_P(StorageContractTest, DestroyingUnfinishedTxnDiscardsWrites) {
  {
    auto txn = storage().BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Put(kMeta, "k", "v").ok());
  }
  EXPECT_FALSE(ReadCommitted(storage(), kMeta, "k").has_value());
}

TEST_P(StorageContractTest, DeleteRemovesKey) {
  MustPut(storage(), kMeta, "k", "v");
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Delete(kMeta, "k").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_FALSE(ReadCommitted(storage(), kMeta, "k").has_value());
}

TEST_P(StorageContractTest, DeletePrefixRemovesOnlyMatchingKeys) {
  PutAll(storage(), kMeta, {{"p", "x"}, {"p1a", "1"}, {"p1b", "2"}, {"p2a", "3"}});
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeletePrefix(kMeta, "p1").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(ScanCommitted(storage(), kMeta), (Rows{{"p", "x"}, {"p2a", "3"}}));
}

TEST_P(StorageContractTest, PutAfterDeletePrefixInSameTxnSurvives) {
  MustPut(storage(), kMeta, "p1a", "old");
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeletePrefix(kMeta, "p1").ok());
  ASSERT_TRUE((*txn)->Put(kMeta, "p1b", "new").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(ScanCommitted(storage(), kMeta), (Rows{{"p1b", "new"}}));
}

TEST_P(StorageContractTest, DeleteRangeIsHalfOpen) {
  PutAll(storage(), kMeta, kAtoE);
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeleteRange(kMeta, "b", "d").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(ScanCommitted(storage(), kMeta), (Rows{{"a", "1"}, {"d", "4"}, {"e", "5"}}));
}

TEST_P(StorageContractTest, DeleteRangeWithEmptyUpperRunsToEnd) {
  PutAll(storage(), kMeta, kAtoE);
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeleteRange(kMeta, "c", "").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(ScanCommitted(storage(), kMeta), (Rows{{"a", "1"}, {"b", "2"}}));
}

TEST_P(StorageContractTest, ColumnFamiliesAreIsolated) {
  MustPut(storage(), kMeta, "k", "meta");
  MustPut(storage(), kSub, "k", "sub");
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeletePrefix(kMeta, "k").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_FALSE(ReadCommitted(storage(), kMeta, "k").has_value());
  EXPECT_EQ(ReadCommitted(storage(), kSub, "k"), "sub");
}

TEST_P(StorageContractTest, IteratorUsesUnsignedBytewiseOrder) {
  PutAll(storage(), kMeta, {{"\xff", "5"}, {"\x80", "4"}, {"a", "2"}, {"\x01", "1"}, {"\x7f", "3"}});
  const Rows got = ScanCommitted(storage(), kMeta);
  ASSERT_EQ(got.size(), 5u);
  EXPECT_EQ(got[0].first, "\x01");
  EXPECT_EQ(got[1].first, "a");
  EXPECT_EQ(got[2].first, "\x7f");
  EXPECT_EQ(got[3].first, "\x80");
  EXPECT_EQ(got[4].first, "\xff");
}

TEST_P(StorageContractTest, IteratorRespectsBounds) {
  PutAll(storage(), kMeta, kAtoE);
  EXPECT_EQ(ScanCommitted(storage(), kMeta, {"b", "d"}), (Rows{{"b", "2"}, {"c", "3"}}));
  EXPECT_EQ(ScanCommitted(storage(), kMeta, {"c", ""}), (Rows{{"c", "3"}, {"d", "4"}, {"e", "5"}}));
  EXPECT_EQ(ScanCommitted(storage(), kMeta, {"", "c"}), (Rows{{"a", "1"}, {"b", "2"}}));
}

TEST_P(StorageContractTest, IteratorSeekAndReverse) {
  PutAll(storage(), kMeta, {{"a", "1"}, {"c", "3"}, {"e", "5"}});
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  auto created = (*view)->NewIterator(kMeta);
  ASSERT_TRUE(created.ok());
  std::unique_ptr<Iterator> it = std::move(created).value();
  ASSERT_TRUE(it->Seek("b").ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
  ASSERT_TRUE(it->Seek("c").ok());
  EXPECT_EQ(it->key(), "c");
  ASSERT_TRUE(it->Seek("f").ok());
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->SeekForPrev("d").ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
  ASSERT_TRUE(it->SeekForPrev("0").ok());
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->SeekToLast().ok());
  EXPECT_EQ(it->key(), "e");
  ASSERT_TRUE(it->Prev().ok());
  EXPECT_EQ(it->key(), "c");
  ASSERT_TRUE(it->Prev().ok());
  EXPECT_EQ(it->key(), "a");
  ASSERT_TRUE(it->Prev().ok());
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(it->Next().code(), StatusCode::kInvalidArgument);
}

TEST_P(StorageContractTest, SeekToLastRespectsUpperBound) {
  PutAll(storage(), kMeta, kAtoE);
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  auto created = (*view)->NewIterator(kMeta, {"", "d"});
  ASSERT_TRUE(created.ok());
  std::unique_ptr<Iterator> it = std::move(created).value();
  ASSERT_TRUE(it->SeekToLast().ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
  ASSERT_TRUE(it->SeekForPrev("z").ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
}

TEST_P(StorageContractTest, SnapshotIsFrozen) {
  MustPut(storage(), kMeta, "k", "1");
  auto snap = storage().BeginSnapshot();
  ASSERT_TRUE(snap.ok());
  MustPut(storage(), kMeta, "k", "2");
  MustPut(storage(), kMeta, "n", "new");
  auto k = (*snap)->Get(kMeta, "k");
  ASSERT_TRUE(k.ok());
  EXPECT_EQ(*k, "1");
  EXPECT_TRUE((*snap)->Get(kMeta, "n").status().IsNotFound());
  EXPECT_EQ(Scan(**snap, kMeta), (Rows{{"k", "1"}}));
  EXPECT_EQ(ReadCommitted(storage(), kMeta, "k"), "2");
}

TEST_P(StorageContractTest, BinarySafeKeysAndValues) {
  const std::string key("a\0b\r\n\xff", 6);
  std::string value(1 << 20, '\0');
  for (size_t i = 0; i < value.size(); ++i) value[i] = static_cast<char>(i * 31);
  MustPut(storage(), kMeta, key, value);
  MustPut(storage(), kMeta, "a", "short");
  EXPECT_EQ(ReadCommitted(storage(), kMeta, key), value);
  EXPECT_EQ(ReadCommitted(storage(), kMeta, "a"), "short");
}

TEST_P(StorageContractTest, EmptyValueIsPresentNotAbsent) {  // Redis: SET k ""
  MustPut(storage(), kMeta, "k", "");
  auto v = ReadCommitted(storage(), kMeta, "k");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "");
}

TEST_P(StorageContractTest, RejectsInvalidArguments) {
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  WriteTxn& t = **txn;
  EXPECT_EQ(t.Put(kMeta, "", "v").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(t.Get(kMeta, "").status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(t.DeletePrefix(kMeta, "").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(t.DeletePrefix(kMeta, std::string(kMaxRangeBoundSize + 1, 'x')).code(),
            StatusCode::kInvalidArgument);
  EXPECT_TRUE(t.DeletePrefix(kMeta, std::string(kMaxRangeBoundSize, 'x')).ok());
  EXPECT_EQ(t.DeleteRange(kMeta, "", "b").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(t.DeleteRange(kMeta, "b", "a").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(t.DeleteRange(kMeta, "b", "b").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(t.Savepoint("").code(), StatusCode::kInvalidArgument);
}

TEST_P(StorageContractTest, SavepointRollbackDiscardsLaterWritesOnly) {
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  WriteTxn& t = **txn;
  ASSERT_TRUE(t.Put(kMeta, "a", "1").ok());
  ASSERT_TRUE(t.Savepoint("s1").ok());
  ASSERT_TRUE(t.Put(kMeta, "b", "2").ok());
  ASSERT_TRUE(t.Delete(kMeta, "a").ok());
  ASSERT_TRUE(t.RollbackToSavepoint("s1").ok());
  EXPECT_TRUE(t.RollbackToSavepoint("missing").IsNotFound());
  ASSERT_TRUE(t.Commit().ok());
  EXPECT_EQ(ReadCommitted(storage(), kMeta, "a"), "1");
  EXPECT_FALSE(ReadCommitted(storage(), kMeta, "b").has_value());
}

TEST_P(StorageContractTest, FinishedTxnRejectsFurtherUse) {
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ((*txn)->Commit().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ((*txn)->Put(kMeta, "k", "v").code(), StatusCode::kInvalidArgument);
}

TEST_P(StorageContractTest, LargeBatchCommitsAtomically) {
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  for (int i = 0; i < 10000; ++i) {
    ASSERT_TRUE((*txn)->Put(kSub, "f" + std::to_string(100000 + i), "v").ok());
  }
  EXPECT_TRUE(ScanCommitted(storage(), kSub).empty());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(ScanCommitted(storage(), kSub).size(), 10000u);
}

std::optional<std::string> TxnGet(ReadView& v, ColumnFamily cf, std::string_view k) {
  auto r = v.Get(cf, k);
  if (!r.ok()) {
    EXPECT_TRUE(r.status().IsNotFound()) << r.status().ToString();
    return std::nullopt;
  }
  return *r;
}

TEST_P(StorageContractTest, TxnReadsAfterDeletePrefix) {
  PutAll(storage(), kMeta, {{"p1a", "1"}, {"p1b", "2"}, {"p2a", "3"}});
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeletePrefix(kMeta, "p1").ok());
  EXPECT_FALSE(TxnGet(**txn, kMeta, "p1a").has_value());
  EXPECT_FALSE(TxnGet(**txn, kMeta, "p1b").has_value());
  EXPECT_EQ(TxnGet(**txn, kMeta, "p2a"), "3");
  EXPECT_EQ(Scan(**txn, kMeta), (Rows{{"p2a", "3"}}));
}

TEST_P(StorageContractTest, TxnReadsAfterDeleteRange) {
  PutAll(storage(), kMeta, kAtoE);
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeleteRange(kMeta, "b", "d").ok());
  EXPECT_FALSE(TxnGet(**txn, kMeta, "b").has_value());
  EXPECT_FALSE(TxnGet(**txn, kMeta, "c").has_value());
  EXPECT_EQ(TxnGet(**txn, kMeta, "a"), "1");
  EXPECT_EQ(TxnGet(**txn, kMeta, "d"), "4");
  EXPECT_EQ(Scan(**txn, kMeta), (Rows{{"a", "1"}, {"d", "4"}, {"e", "5"}}));
}

TEST_P(StorageContractTest, BufferedPutThenDeletePrefixIsDeleted) {
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Put(kMeta, "p1k", "v").ok());
  ASSERT_TRUE((*txn)->DeletePrefix(kMeta, "p1").ok());
  EXPECT_FALSE(TxnGet(**txn, kMeta, "p1k").has_value());
  EXPECT_TRUE(Scan(**txn, kMeta).empty());
}

TEST_P(StorageContractTest, DeleteThenPutSameKeyReadsNewValue) {
  MustPut(storage(), kMeta, "k", "old");
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Delete(kMeta, "k").ok());
  ASSERT_TRUE((*txn)->Put(kMeta, "k", "v").ok());
  EXPECT_EQ(TxnGet(**txn, kMeta, "k"), "v");
  EXPECT_EQ(Scan(**txn, kMeta), (Rows{{"k", "v"}}));
}

TEST_P(StorageContractTest, PutAfterDeletePrefixVisibleInsideTxn) {
  MustPut(storage(), kMeta, "pa", "old");
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->DeletePrefix(kMeta, "p").ok());
  ASSERT_TRUE((*txn)->Put(kMeta, "px", "new").ok());
  EXPECT_EQ(TxnGet(**txn, kMeta, "px"), "new");
  EXPECT_FALSE(TxnGet(**txn, kMeta, "pa").has_value());
  EXPECT_EQ(Scan(**txn, kMeta), (Rows{{"px", "new"}}));
}

TEST_P(StorageContractTest, ReadViewObservesLaterCommits) {
  MustPut(storage(), kMeta, "k", "1");
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  EXPECT_EQ(TxnGet(**view, kMeta, "k"), "1");
  MustPut(storage(), kMeta, "k", "2");
  EXPECT_EQ(TxnGet(**view, kMeta, "k"), "2");
}

TEST_P(StorageContractTest, WriteTxnObservesLaterCommits) {
  MustPut(storage(), kMeta, "k", "1");
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  EXPECT_EQ(TxnGet(**txn, kMeta, "k"), "1");
  {
    auto other = storage().BeginWrite();
    ASSERT_TRUE(other.ok());
    ASSERT_TRUE((*other)->Put(kMeta, "k", "2").ok());
    ASSERT_TRUE((*other)->Commit().ok());
  }
  EXPECT_EQ(TxnGet(**txn, kMeta, "k"), "2");
}

TEST_P(StorageContractTest, SavepointRollbackRestoresRangeDeletes) {
  PutAll(storage(), kMeta, {{"b", "2"}});
  auto txn = storage().BeginWrite();
  ASSERT_TRUE(txn.ok());
  WriteTxn& t = **txn;
  ASSERT_TRUE(t.Put(kMeta, "a1", "1").ok());
  ASSERT_TRUE(t.Savepoint("s1").ok());
  ASSERT_TRUE(t.DeletePrefix(kMeta, "a").ok());
  ASSERT_TRUE(t.DeleteRange(kMeta, "b", "c").ok());
  EXPECT_FALSE(TxnGet(t, kMeta, "a1").has_value());
  EXPECT_FALSE(TxnGet(t, kMeta, "b").has_value());
  ASSERT_TRUE(t.RollbackToSavepoint("s1").ok());
  EXPECT_EQ(TxnGet(t, kMeta, "a1"), "1");
  EXPECT_EQ(TxnGet(t, kMeta, "b"), "2");
  ASSERT_TRUE(t.Commit().ok());
  EXPECT_EQ(ScanCommitted(storage(), kMeta), (Rows{{"a1", "1"}, {"b", "2"}}));
}

TEST_P(StorageContractTest, IteratorLowerBoundClampsSeeks) {
  PutAll(storage(), kMeta, kAtoE);
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  auto created = (*view)->NewIterator(kMeta, {"b", "d"});
  ASSERT_TRUE(created.ok());
  std::unique_ptr<Iterator> it = std::move(created).value();
  ASSERT_TRUE(it->Seek("a").ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  ASSERT_TRUE(it->Prev().ok());
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->SeekForPrev("a").ok());
  EXPECT_FALSE(it->Valid());
}

TEST_P(StorageContractTest, LowerOnlyBoundStopsReverseIteration) {
  PutAll(storage(), kMeta, kAtoE);
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  auto created = (*view)->NewIterator(kMeta, {"b", ""});
  ASSERT_TRUE(created.ok());
  std::unique_ptr<Iterator> it = std::move(created).value();
  ASSERT_TRUE(it->SeekForPrev("aa").ok());  // "a" is <= the target but below lower
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->SeekForPrev("bb").ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  ASSERT_TRUE(it->Prev().ok());  // steps onto "a", below lower
  EXPECT_FALSE(it->Valid());
  ASSERT_TRUE(it->SeekToLast().ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "e");
}

TEST_P(StorageContractTest, IteratorBoundsWithHighBytes) {
  PutAll(storage(), kMeta, {{"\x7f", "1"}, {"\x80", "2"}, {"\x81", "3"}, {"\xff", "4"}});
  EXPECT_EQ(ScanCommitted(storage(), kMeta, {"\x80", "\xff"}), (Rows{{"\x80", "2"}, {"\x81", "3"}}));
}

// Seek("") == SeekToFirst; SeekForPrev("") is OK and leaves the iterator invalid.
void ExpectEmptySeekTargets(ReadView& view, IterOptions opts, std::string_view first) {
  auto created = view.NewIterator(kMeta, std::move(opts));
  ASSERT_TRUE(created.ok());
  std::unique_ptr<Iterator> it = std::move(created).value();
  ASSERT_TRUE(it->Seek("").ok());
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), first);
  ASSERT_TRUE(it->SeekForPrev("").ok());
  EXPECT_FALSE(it->Valid());
}

TEST_P(StorageContractTest, EmptySeekTargetsWithoutBounds) {
  PutAll(storage(), kMeta, kAtoE);
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  ExpectEmptySeekTargets(**view, {}, "a");
  ExpectEmptySeekTargets(**view, {"", "d"}, "a");
}

TEST_P(StorageContractTest, EmptySeekTargetsWithBounds) {
  PutAll(storage(), kMeta, kAtoE);
  auto view = storage().BeginRead();
  ASSERT_TRUE(view.ok());
  ExpectEmptySeekTargets(**view, {"b", ""}, "b");
  ExpectEmptySeekTargets(**view, {"b", "d"}, "b");
}

TEST_P(StorageContractTest, ReopenPreservesCommittedData) {
  if (!harness_->SupportsReopen()) GTEST_SKIP() << "backend is not persistent";
  MustPut(storage(), kMeta, "k", "v");
  MustPut(storage(), ColumnFamily::kSys, "oid_hwm", "42");
  harness_->Reopen();
  EXPECT_EQ(ReadCommitted(storage(), kMeta, "k"), "v");
  EXPECT_EQ(ReadCommitted(storage(), ColumnFamily::kSys, "oid_hwm"), "42");
}

}  // namespace
}  // namespace tideskv::test
