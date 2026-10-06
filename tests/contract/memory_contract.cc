#include "contract/storage_contract.h"
#include "storage/memory_storage.h"

namespace tideskv::test {
namespace {

class MemoryHarness final : public StorageHarness {
 public:
  Storage& storage() override { return storage_; }
  bool SupportsReopen() const override { return false; }
  void Reopen() override {}

 private:
  InMemoryStorage storage_;
};

INSTANTIATE_TEST_SUITE_P(
    Memory, StorageContractTest,
    ::testing::Values(ContractParam{
        "Memory", [] { return std::unique_ptr<StorageHarness>(std::make_unique<MemoryHarness>()); }}),
    ContractParamName);

}  // namespace
}  // namespace tideskv::test
