#include "types/scan.h"

#include "common/crc16.h"
#include "common/glob.h"
#include "common/string_util.h"
#include "encoding/key_codec.h"
#include "encoding/key_layout.h"

namespace tideskv {
namespace {

using encoding::ValueType;

IterOptions DbRange(uint8_t db) {
  return {encoding::DbPrefix(db), encoding::DbPrefix(static_cast<uint8_t>(db + 1))};
}

// Decodes the current entry. *live is false for expired objects.
Status Current(const Iterator& it, int64_t now_ms, std::string* user_key, ValueType* type, bool* live) {
  encoding::MetaKey mk;
  if (!encoding::DecodeMetaKey(it.key(), &mk)) return Status::Corruption("bad meta key");
  auto meta = encoding::DecodeMetadata(it.value());
  if (!meta.ok()) return meta.status();
  *user_key = std::string(mk.user_key);
  *type = meta->meta.type;
  *live = !meta->meta.IsExpired(now_ms);
  return Status::Ok();
}

bool Matches(const ScanFilter& f, std::string_view key, ValueType type) {
  if (f.match_nothing) return false;
  if (f.type && *f.type != type) return false;
  return !f.pattern || GlobMatch(*f.pattern, key);
}

// Positions `it` after the key matching a truncated position: walks the keys starting with the
// saved prefix; if none has the full key's size and digest (deleted since), resumes at the
// prefix, which can repeat keys but never skips one.
// The walk costs O(keys sharing the prefix) per call and is not bounded by COUNT.
// TODO(M8): cap the walk and fall back to resuming at the prefix (a repeat) past the cap.
// A writer that deletes the last-examined huge key before every call can stall progress
// within the group (each resume falls back to the prefix); SCAN's guarantees allow this.
Status SeekPastTruncated(Iterator& it, const ScanPosition& pos) {
  Status st = it.Seek(pos.key);
  for (; st.ok() && it.Valid() && it.key().starts_with(pos.key); st = it.Next()) {
    const std::string_view key = it.key();
    if (key.size() == pos.full_size && PositionDigest(key) == pos.digest) return it.Next();
  }
  if (!st.ok()) return st;
  return it.Seek(pos.key);
}

// Up to kRandomKeyBatch live keys of db strictly after the meta key `after` (empty = from the first).
Status LiveKeysAfter(ReadView& view, uint8_t db, const std::string& after, int64_t now_ms,
                     std::vector<std::string>* out) {
  out->clear();
  auto it = view.NewIterator(ColumnFamily::kMeta, DbRange(db));
  if (!it.ok()) return it.status();
  Status st = after.empty() ? (*it)->SeekToFirst() : (*it)->Seek(after);
  if (st.ok() && !after.empty() && (*it)->Valid() && (*it)->key() == after) st = (*it)->Next();
  for (; st.ok() && (*it)->Valid() && out->size() < kRandomKeyBatch; st = (*it)->Next()) {
    std::string key;
    ValueType type{};
    bool live = false;
    if (Status cur = Current(**it, now_ms, &key, &type, &live); !cur.ok()) return cur;
    if (live) out->push_back(std::move(key));
  }
  return st;
}

Result<std::optional<std::string>> RandomKeyFromCursor(ReadView& view, uint8_t db, int64_t now_ms,
                                                       uint64_t random, std::string_view cursor) {
  std::vector<std::string> keys;
  const std::string after = cursor.empty() ? std::string() : encoding::MetaKeyFor(db, cursor);
  if (Status st = LiveKeysAfter(view, db, after, now_ms, &keys); !st.ok()) return st;
  if (keys.empty() && !after.empty()) {  // the end of the db: start over
    if (Status st = LiveKeysAfter(view, db, std::string(), now_ms, &keys); !st.ok()) return st;
  }
  if (keys.empty()) return std::optional<std::string>();
  return std::optional<std::string>(std::move(keys[random % keys.size()]));
}

}  // namespace

// Positions `it` at the first entry strictly after `pos` (empty key = the first entry).
Status SeekPast(Iterator& it, const ScanPosition& pos) {
  if (pos.key.empty()) return it.SeekToFirst();
  if (pos.truncated) return SeekPastTruncated(it, pos);
  Status st = it.Seek(pos.key);
  if (st.ok() && it.Valid() && it.key() == pos.key) st = it.Next();
  return st;
}

std::optional<ValueType> TypeFromName(std::string_view name) {
  for (ValueType t :
       {ValueType::kString, ValueType::kHash, ValueType::kList, ValueType::kSet, ValueType::kZSet}) {
    if (EqualsIgnoreCase(name, encoding::TypeName(t))) return t;
  }
  return std::nullopt;
}

Status ScanKeys(ReadView& view, uint8_t db, const ScanPosition& position, size_t count,
                const ScanFilter& filter, int64_t now_ms, ScanPage* out) {
  *out = ScanPage{};
  auto it = view.NewIterator(ColumnFamily::kMeta, DbRange(db));
  if (!it.ok()) return it.status();
  Status st = SeekPast(**it, position);
  for (size_t examined = 0; st.ok() && (*it)->Valid() && examined < count; ++examined) {
    std::string key;
    ValueType type{};
    bool live = false;
    if (st = Current(**it, now_ms, &key, &type, &live); !st.ok()) return st;
    if (live && Matches(filter, key, type)) out->keys.push_back(std::move(key));
    out->next_position = std::string((*it)->key());
    st = (*it)->Next();
  }
  if (!st.ok()) return st;
  if (!(*it)->Valid()) out->next_position.clear();  // nothing left: iteration finished
  return Status::Ok();
}

Status MatchingKeys(ReadView& view, uint8_t db, std::string_view pattern, int64_t now_ms,
                    std::vector<std::string>* out) {
  out->clear();
  auto it = view.NewIterator(ColumnFamily::kMeta, DbRange(db));
  if (!it.ok()) return it.status();
  ScanFilter filter;
  if (pattern != "*") filter.pattern = pattern;  // Redis 7.2 keysCommand: only "*" means all keys
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    std::string key;
    ValueType type{};
    bool live = false;
    if (Status cur = Current(**it, now_ms, &key, &type, &live); !cur.ok()) return cur;
    if (live && Matches(filter, key, type)) out->push_back(std::move(key));
  }
  return st;
}

Result<std::optional<std::string>> RandomKey(ReadView& view, uint8_t db, int64_t now_ms, uint64_t random,
                                             std::string_view cursor) {
  if (encoding::Slotless()) return RandomKeyFromCursor(view, db, now_ms, random, cursor);
  const std::string start = encoding::MetaSlotPrefix(db, static_cast<uint16_t>(random % kSlotCount));
  auto it = view.NewIterator(ColumnFamily::kMeta, DbRange(db));
  if (!it.ok()) return it.status();
  // Pass 0: [start, end of db); pass 1: [begin of db, start).
  for (int pass = 0; pass < 2; ++pass) {
    Status st = pass == 0 ? (*it)->Seek(start) : (*it)->SeekToFirst();
    for (; st.ok() && (*it)->Valid() && (pass == 0 || (*it)->key() < start); st = (*it)->Next()) {
      std::string key;
      ValueType type{};
      bool live = false;
      if (Status cur = Current(**it, now_ms, &key, &type, &live); !cur.ok()) return cur;
      if (live) return std::optional<std::string>(std::move(key));
    }
    if (!st.ok()) return st;
  }
  return std::optional<std::string>();
}

Status ScanElements(ReadView& view, std::string_view prefix, const ScanPosition& position, size_t count,
                    std::optional<std::string_view> pattern, bool with_values, ElementPage* out) {
  *out = ElementPage{};
  auto it =
      view.NewIterator(ColumnFamily::kSubkey, {std::string(prefix), encoding::PrefixUpperBound(prefix)});
  if (!it.ok()) return it.status();
  Status st = SeekPast(**it, position);
  for (size_t examined = 0; st.ok() && (*it)->Valid() && examined < count; ++examined) {
    const std::string_view key = (*it)->key();
    const std::string_view field = key.substr(prefix.size());
    if (!pattern || GlobMatch(*pattern, field)) {
      out->items.emplace_back(field);
      if (with_values) out->items.emplace_back((*it)->value());
    }
    out->next_position = std::string(key);
    st = (*it)->Next();
  }
  if (!st.ok()) return st;
  if (!(*it)->Valid()) out->next_position.clear();  // nothing left
  return Status::Ok();
}

}  // namespace tideskv
