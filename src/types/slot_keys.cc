#include "types/slot_keys.h"

#include <functional>
#include <utility>

#include "common/crc16.h"
#include "encoding/key_codec.h"

namespace tideskv {
namespace {

Result<bool> AnyMeta(ReadView& view, IterOptions range) {
  auto it = view.NewIterator(ColumnFamily::kMeta, std::move(range));
  if (!it.ok()) return it.status();
  if (Status st = (*it)->SeekToFirst(); !st.ok()) return st;  // reaching the end is OK, not an error
  return (*it)->Valid();
}

// Calls fn(user_key) for the slot's meta records in db 0 until fn returns false. [0][slot] .. [0][slot+1]:
// valid for 16383 too ([0][0x40][0x00] sorts after every slot).
Status ForSlotKeys(ReadView& view, uint16_t slot, const std::function<bool(std::string_view)>& fn) {
  auto it = view.NewIterator(
      ColumnFamily::kMeta,
      {encoding::MetaSlotPrefix(0, slot), encoding::MetaSlotPrefix(0, static_cast<uint16_t>(slot + 1))});
  if (!it.ok()) return it.status();
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    encoding::MetaKey mk;
    if (!encoding::DecodeMetaKey((*it)->key(), &mk)) return Status::Corruption("undecodable meta key");
    if (!fn(mk.user_key)) return Status::Ok();
  }
  return st.IsNotFound() ? Status::Ok() : st;
}

}  // namespace

Result<bool> HasKeysOutsideDb0(ReadView& view) { return AnyMeta(view, {encoding::DbPrefix(1), ""}); }

Result<bool> KeyspaceEmpty(ReadView& view) {
  auto any = AnyMeta(view, {});
  if (!any.ok()) return any.status();
  return !*any;
}

Result<uint64_t> CountKeysInSlot(ReadView& view, uint16_t slot) {
  uint64_t n = 0;
  const Status st = ForSlotKeys(view, slot, [&n](std::string_view) {
    ++n;
    return true;
  });
  if (!st.ok()) return st;
  return n;
}

Result<std::vector<std::string>> KeysInSlot(ReadView& view, uint16_t slot, uint64_t count) {
  std::vector<std::string> keys;
  if (count == 0) return keys;
  const Status st = ForSlotKeys(view, slot, [&](std::string_view k) {
    keys.emplace_back(k);
    return keys.size() < count;
  });
  if (!st.ok()) return st;
  return keys;
}

IterOptions SlotRangeBounds(uint16_t first, uint16_t last) {
  return {encoding::MetaSlotPrefix(0, first), encoding::MetaSlotPrefix(0, static_cast<uint16_t>(last + 1))};
}

}  // namespace tideskv
