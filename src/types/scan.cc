#include "types/scan.h"

#include "common/crc16.h"
#include "common/glob.h"
#include "common/string_util.h"
#include "encoding/key_codec.h"

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
Status SeekPastTruncated(Iterator& it, const ScanPosition& pos) {
  Status st = it.Seek(pos.key);
  for (; st.ok() && it.Valid() && it.key().starts_with(pos.key); st = it.Next()) {
    const std::string_view key = it.key();
    if (key.size() == pos.full_size && PositionDigest(key) == pos.digest) return it.Next();
  }
  if (!st.ok()) return st;
  return it.Seek(pos.key);
}

// Positions `it` at the first entry strictly after `pos` (empty key = the first entry).
Status SeekPast(Iterator& it, const ScanPosition& pos) {
  if (pos.key.empty()) return it.SeekToFirst();
  if (pos.truncated) return SeekPastTruncated(it, pos);
  Status st = it.Seek(pos.key);
  if (st.ok() && it.Valid() && it.key() == pos.key) st = it.Next();
  return st;
}

}  // namespace

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

Result<std::optional<std::string>> RandomKey(ReadView& view, uint8_t db, int64_t now_ms, uint64_t random) {
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

}  // namespace tideskv
