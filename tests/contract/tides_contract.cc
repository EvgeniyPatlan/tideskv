#include <cstdio>
#include <cstdlib>

#include "contract/storage_contract.h"
#include "storage/tides_storage.h"
#include "support/temp_dir.h"

namespace tideskv::test {
namespace {

class TidesHarness final : public StorageHarness {
 public:
  TidesHarness() { Open(); }
  Storage& storage() override { return *storage_; }
  bool SupportsReopen() const override { return true; }
  void Reopen() override {
    storage_.reset();
    Open();
  }

 private:
  void Open() {
    StorageOptions o;
    o.path = (dir_.path() / "db").string();
    o.sync_mode = SyncMode::kNone;  // tests don't need fsync; clean close loses nothing
    o.flush_threads = 1;
    o.compaction_threads = 1;
    auto opened = TidesStorage::Open(o);
    if (!opened.ok()) {
      std::fprintf(stderr, "TidesStorage::Open failed: %s\n", opened.status().ToString().c_str());
      std::abort();
    }
    storage_ = std::move(opened).value();
  }

  TempDir dir_{"tideskv-contract"};       // declared first: destroyed after storage_
  std::unique_ptr<TidesStorage> storage_;
};

INSTANTIATE_TEST_SUITE_P(Tides, StorageContractTest,
                         ::testing::Values(ContractParam{
                             "Tides", [] { return std::unique_ptr<StorageHarness>(std::make_unique<TidesHarness>()); }}),
                         ContractParamName);

}  // namespace
}  // namespace tideskv::test
