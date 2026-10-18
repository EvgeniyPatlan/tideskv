#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "encoding/meta_codec.h"
#include "storage/storage.h"
#include "types/object_store.h"
#include "types/oid_allocator.h"

// Shared logic of the collection types (hash, set; M3 plan E2). A collection is one meta
// record {type, oid, size} plus one subkey entry per element: [db][slot][oid][field] -> value.
// meta.size is kept exact, and a collection with 0 elements is never stored.
namespace tideskv {

struct Collection {
  uint8_t db = 0;
  std::string key;
  uint16_t slot = 0;
  StoredObject obj;     // state kLive or kMissing, never kExpired
  bool stored = false;  // a meta record exists for it in storage (set by the Load* functions)
  // The meta record as stored (encoded); empty when none is. SaveCollection skips rewriting it unchanged.
  std::string stored_meta;

  bool exists() const { return obj.live(); }
  uint64_t size() const { return exists() ? obj.meta.size : 0; }
  std::string Prefix() const;                           // SubkeyPrefix(db, slot, oid)
  std::string SubkeyFor(std::string_view field) const;  // EncodeSubkey(db, slot, oid, field)
};

// For readers: a missing or expired key is an empty collection (!exists()); a live key of
// another type is UserError(kErrWrongType).
Result<Collection> LoadCollection(ReadView& view, uint8_t db, std::string_view key, encoding::ValueType type,
                                  int64_t now_ms);
// For writers: like LoadCollection, but an expired object is deleted in txn first (LoadForWrite).
Result<Collection> LoadCollectionForWrite(WriteTxn& txn, uint8_t db, std::string_view key,
                                          encoding::ValueType type, int64_t now_ms);
// Turns a missing collection into a new, empty one with a fresh oid. Nothing is written until
// SaveCollection; an oid taken by a command that then fails is simply never used.
Status InitCollection(Collection* c, encoding::ValueType type, OidAllocator& oids);
// Persists c after its size changed: the meta record, or, at size 0, deletes the key
// (DeleteEmptyObject) if it was stored. A meta record identical to the stored one is not rewritten
// (txn.NoteRewrite still reports the key as written, for WATCH): those rewrites only fed the meta CF
// small flushes that each cost a compaction of all the keyspace's metadata. Updates c.stored_meta.
Status SaveCollection(WriteTxn& txn, Collection& c);
// LoadCollectionForWrite, then InitCollection if the key is missing (HSET, SADD, ...).
Result<Collection> OpenCollectionForWrite(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                                          encoding::ValueType type, int64_t now_ms);

Result<std::optional<std::string>> GetElement(ReadView& view, const Collection& c, std::string_view field);
Result<bool> HasElement(ReadView& view, const Collection& c, std::string_view field);
// Writes field = value, counting a new field into c's size. Returns whether the field was new.
Result<bool> PutElement(WriteTxn& txn, Collection& c, std::string_view field, std::string_view value);
// Deletes field if present, decrementing c's size. Returns whether it was present. Sees this
// transaction's earlier writes, so a repeated field is removed once.
Result<bool> RemoveElement(WriteTxn& txn, Collection& c, std::string_view field);

// Visits the elements in byte order of their fields; fn returns false to stop early.
using ElementFn = std::function<bool(std::string_view field, std::string_view value)>;
Status ForEachElement(ReadView& view, const Collection& c, const ElementFn& fn);
// One pass visiting the element at each position of `sorted_positions` (ascending; a repeated
// position is visited repeatedly). Corruption if the collection holds fewer elements than its size.
Status IterateRange(ReadView& view, const Collection& c, const std::vector<uint64_t>& sorted_positions,
                    const std::function<void(std::string_view field, std::string_view value)>& fn);

struct Element {
  std::string field;
  std::string value;
};

// count >= 0: min(count, size) distinct elements in byte order; count < 0: |count| picks with
// repeats, in random order (M3 plan E3). count must not be INT64_MIN. One pass, O(size).
Result<std::vector<Element>> RandomElements(ReadView& view, const Collection& c, int64_t count,
                                            std::mt19937_64& rng);

}  // namespace tideskv
