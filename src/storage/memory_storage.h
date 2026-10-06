#pragma once
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "storage/storage.h"

namespace tideskv {

// Test fake. Copy-on-write: every commit copies the tables, so snapshots are just
// shared_ptrs. Correct and simple, deliberately not fast.
class InMemoryStorage final : public Storage {
 public:
  using Table = std::map<std::string, std::string, std::less<>>;
  using Tables = std::array<Table, kColumnFamilyCount>;

  struct Op {
    enum class Kind : uint8_t { kPut, kDelete, kDeletePrefix, kDeleteRange };
    Kind kind;
    ColumnFamily cf;
    std::string a;  // key, prefix or range lo
    std::string b;  // value or range hi
  };

  InMemoryStorage();

  Result<std::unique_ptr<WriteTxn>> BeginWrite() override;
  Result<std::unique_ptr<ReadView>> BeginRead() override;
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override;

  // Used by the transaction/view implementations.
  std::shared_ptr<const Tables> Current() const;
  void Apply(const std::vector<Op>& ops);

 private:
  mutable std::mutex mu_;
  std::shared_ptr<const Tables> tables_;
};

}  // namespace tideskv
