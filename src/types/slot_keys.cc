#include "types/slot_keys.h"

#include <utility>

#include "encoding/key_codec.h"

namespace tideskv {
namespace {

Result<bool> AnyMeta(ReadView& view, IterOptions range) {
  auto it = view.NewIterator(ColumnFamily::kMeta, std::move(range));
  if (!it.ok()) return it.status();
  if (Status st = (*it)->SeekToFirst(); !st.ok()) return st;  // reaching the end is OK, not an error
  return (*it)->Valid();
}

}  // namespace

Result<bool> HasKeysOutsideDb0(ReadView& view) { return AnyMeta(view, {encoding::DbPrefix(1), ""}); }

Result<bool> KeyspaceEmpty(ReadView& view) {
  auto any = AnyMeta(view, {});
  if (!any.ok()) return any.status();
  return !*any;
}

}  // namespace tideskv
