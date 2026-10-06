#include "storage/tides_status.h"

#include <string>

extern "C" {
#include <db.h>
}

namespace tideskv {

Status FromTdb(int rc, std::string_view context) {
  if (rc == TDB_SUCCESS) return Status::Ok();
  std::string msg = std::string(context) + ": tidesdb error " + std::to_string(rc);
  switch (rc) {
    case TDB_ERR_NOT_FOUND: return Status::NotFound(std::move(msg));
    case TDB_ERR_INVALID_ARGS: return Status::InvalidArgument(std::move(msg));
    case TDB_ERR_LOCKED: return Status::Busy(std::move(msg));
    case TDB_ERR_CONFLICT:
    case TDB_ERR_TXN_ABORTED: return Status::Conflict(std::move(msg));
    case TDB_ERR_NO_SPACE: return Status::NoSpace(std::move(msg));
    case TDB_ERR_IO: return Status::IOError(std::move(msg));
    case TDB_ERR_CORRUPTION: return Status::Corruption(std::move(msg));
    default: return Status::Internal(std::move(msg));
  }
}

}  // namespace tideskv
