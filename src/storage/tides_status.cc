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
    case TDB_ERR_NOT_FOUND:
      return Status::NotFound(std::move(msg));
    case TDB_ERR_INVALID_ARGS:
    case TDB_ERR_TOO_LARGE:
      return Status::InvalidArgument(std::move(msg));
    case TDB_ERR_LOCKED:
    case TDB_ERR_TOO_OLD:
      return Status::Busy(std::move(msg));
    case TDB_ERR_CONFLICT:
    case TDB_ERR_TXN_ABORTED:
    case TDB_ERR_TXN_EXPIRED:
      return Status::Conflict(std::move(msg));
    case TDB_ERR_NO_SPACE:
    case TDB_ERR_MEMORY_LIMIT:
      return Status::NoSpace(std::move(msg));
    case TDB_ERR_IO:
    case TDB_ERR_READONLY:
      return Status::IOError(std::move(msg));
    case TDB_ERR_CORRUPTION:
      return Status::Corruption(std::move(msg));
    default:
      return Status::Internal(std::move(msg));
  }
}

}  // namespace tideskv
