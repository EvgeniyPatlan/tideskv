#include "repl/replog_reader.h"

#include <string>

#include "encoding/replog_codec.h"
#include "repl/repl_errors.h"

namespace tideskv::repl {

Status ReplogReader::Read(uint64_t last, size_t max_bytes, const BatchFn& fn) {
  if (next_ > last) return Status::Ok();
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  auto it = (*view)->NewIterator(ColumnFamily::kReplog,
                                 IterOptions{encoding::ReplogKey(next_), encoding::ReplogKey(last + 1)});
  if (!it.ok()) return it.status();
  size_t bytes = 0;
  bool any = false;
  Status st = (*it)->Seek(encoding::ReplogKey(next_));
  while (st.ok() && (*it)->Valid() && next_ <= last && bytes < max_bytes) {
    uint64_t seq = 0;
    if (!encoding::DecodeReplogKey((*it)->key(), &seq) || seq != next_) break;  // a gap: reported below
    if (Status s = fn(seq, (*it)->value()); !s.ok()) return s;
    bytes += (*it)->value().size();
    any = true;
    ++next_;
    st = (*it)->Next();
  }
  if (!st.ok() && !st.IsNotFound()) return st;
  if (!any)
    return Status::NotFound(std::string(kMsgReplogEntryPrefix) + std::to_string(next_) +
                            std::string(kMsgReplogEntryNotRetained));
  return Status::Ok();
}

}  // namespace tideskv::repl
