#include "types/string_type.h"

#include "types/errors.h"
#include "types/object_store.h"

namespace tideskv {
namespace {

using Condition = SetOptions::Condition;

Status WrongType() { return Status::UserError(std::string(kErrWrongType)); }

}  // namespace

Result<std::optional<std::string>> StringGet(ReadView& view, uint8_t db, std::string_view key,
                                             int64_t now_ms) {
  auto obj = LoadObject(view, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (!obj->live()) return std::optional<std::string>();
  if (!obj->IsString()) return WrongType();
  return std::optional<std::string>(std::move(obj->payload));
}

Status StringSet(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view value,
                 const SetOptions& opts, int64_t now_ms, SetResult* out) {
  *out = SetResult{};
  auto old = LoadForWrite(txn, db, key, now_ms);
  if (!old.ok()) return old.status();
  if (opts.get && old->live()) {
    if (!old->IsString()) return WrongType();
    out->old_value = old->payload;
  }
  if ((opts.condition == Condition::kIfMissing && old->live()) ||
      (opts.condition == Condition::kIfExists && !old->live())) {
    return Status::Ok();
  }
  out->written = true;
  const int64_t expire_ms = opts.keep_ttl ? (old->live() ? old->meta.expire_ms : 0) : opts.expire_ms;
  if (expire_ms != 0 && expire_ms <= now_ms) {  // EXAT/PXAT in the past: set, then already expired
    return old->live() ? DeleteObject(txn, db, key, old->meta) : Status::Ok();
  }
  return WriteString(txn, db, key, value, expire_ms, *old);
}

Result<std::optional<std::string>> StringGetDel(WriteTxn& txn, uint8_t db, std::string_view key,
                                                int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (!obj->live()) return std::optional<std::string>();
  if (!obj->IsString()) return WrongType();
  if (Status st = DeleteObject(txn, db, key, obj->meta); !st.ok()) return st;
  return std::optional<std::string>(std::move(obj->payload));
}

Result<bool> StringMultiSet(WriteTxn& txn, uint8_t db, const std::vector<KeyValue>& pairs,
                            bool only_if_none_exist, int64_t now_ms) {
  if (only_if_none_exist) {
    for (const auto& [key, value] : pairs) {
      auto obj = LoadForWrite(txn, db, key, now_ms);
      if (!obj.ok()) return obj.status();
      if (obj->live()) return false;
    }
  }
  for (const auto& [key, value] : pairs) {
    auto old = LoadForWrite(txn, db, key, now_ms);
    if (!old.ok()) return old.status();
    if (Status st = WriteString(txn, db, key, value, 0, *old); !st.ok()) return st;
  }
  return true;
}

Result<std::vector<std::optional<std::string>>> StringMultiGet(ReadView& view, uint8_t db,
                                                               const std::vector<std::string_view>& keys,
                                                               int64_t now_ms) {
  std::vector<std::optional<std::string>> out;
  out.reserve(keys.size());
  for (std::string_view key : keys) {
    auto obj = LoadObject(view, db, key, now_ms);
    if (!obj.ok()) return obj.status();
    out.push_back(obj->IsString() ? std::optional<std::string>(std::move(obj->payload)) : std::nullopt);
  }
  return out;
}

}  // namespace tideskv
