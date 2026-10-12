#include "types/string_type.h"

#include <cmath>
#include <limits>

#include "common/number_format.h"
#include "common/string_util.h"
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

Result<std::optional<std::string>> StringGetEx(WriteTxn& txn, uint8_t db, std::string_view key,
                                               const GetExResolver& resolve, int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (!obj->live()) return std::optional<std::string>();
  if (!obj->IsString()) return WrongType();
  auto change = resolve();
  if (!change.ok()) return change.status();
  Status st;
  if (change->kind == GetExChange::Kind::kSetExpire) {
    st = change->expire_ms <= now_ms ? DeleteObject(txn, db, key, obj->meta)
                                     : SetObjectExpire(txn, db, key, *obj, change->expire_ms);
  } else if (change->kind == GetExChange::Kind::kPersist && obj->meta.expire_ms != 0) {
    st = SetObjectExpire(txn, db, key, *obj, 0);
  }
  if (!st.ok()) return st;
  return std::optional<std::string>(std::move(obj->payload));
}

Result<int64_t> StringAppend(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view tail,
                             int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (obj->live() && !obj->IsString()) return WrongType();
  if (obj->payload.size() + tail.size() > kMaxStringSize)
    return Status::UserError(std::string(kErrStringTooLong));
  std::string value = std::move(obj->payload);
  value.append(tail);
  const int64_t expire_ms = obj->live() ? obj->meta.expire_ms : 0;
  if (Status st = WriteString(txn, db, key, value, expire_ms, *obj); !st.ok()) return st;
  return static_cast<int64_t>(value.size());
}

Result<int64_t> StringSetRange(WriteTxn& txn, uint8_t db, std::string_view key, int64_t offset,
                               std::string_view value, int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (obj->live() && !obj->IsString()) return WrongType();
  std::string current = std::move(obj->payload);
  if (value.empty()) return static_cast<int64_t>(current.size());
  if (static_cast<uint64_t>(offset) + value.size() > kMaxStringSize) {
    return Status::UserError(std::string(kErrStringTooLong));
  }
  const size_t end = static_cast<size_t>(offset) + value.size();
  if (current.size() < end) current.resize(end, '\0');
  current.replace(static_cast<size_t>(offset), value.size(), value);
  const int64_t expire_ms = obj->live() ? obj->meta.expire_ms : 0;
  if (Status st = WriteString(txn, db, key, current, expire_ms, *obj); !st.ok()) return st;
  return static_cast<int64_t>(current.size());
}

std::string_view RedisRange(std::string_view s, int64_t start, int64_t end) {
  const auto len = static_cast<int64_t>(s.size());
  if (start < 0 && end < 0 && start > end) return {};
  if (start < 0) start += len;
  if (end < 0) end += len;
  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if (end >= len) end = len - 1;
  if (start > end || len == 0) return {};
  return s.substr(static_cast<size_t>(start), static_cast<size_t>(end - start + 1));
}

Result<int64_t> StringIncrBy(WriteTxn& txn, uint8_t db, std::string_view key, int64_t delta, int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (obj->live() && !obj->IsString()) return WrongType();
  int64_t value = 0;
  if (obj->live() && !ParseInt64(obj->payload, &value)) return Status::UserError(std::string(kErrNotInteger));
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  if ((delta < 0 && value < 0 && delta < kMin - value) || (delta > 0 && value > 0 && delta > kMax - value)) {
    return Status::UserError(std::string(kErrOverflow));
  }
  value += delta;
  const int64_t expire_ms = obj->live() ? obj->meta.expire_ms : 0;
  if (Status st = WriteString(txn, db, key, std::to_string(value), expire_ms, *obj); !st.ok()) return st;
  return value;
}

Result<std::string> StringIncrByFloat(WriteTxn& txn, uint8_t db, std::string_view key, long double delta,
                                      int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (obj->live() && !obj->IsString()) return WrongType();
  long double value = 0;
  if (obj->live() && !ParseLongDouble(obj->payload, &value))
    return Status::UserError(std::string(kErrNotFloat));
  value += delta;
  if (std::isnan(value) || std::isinf(value)) return Status::UserError(std::string(kErrNanOrInfinity));
  std::string text = FormatLongDouble(value);
  const int64_t expire_ms = obj->live() ? obj->meta.expire_ms : 0;
  if (Status st = WriteString(txn, db, key, text, expire_ms, *obj); !st.ok()) return st;
  return text;
}

}  // namespace tideskv
