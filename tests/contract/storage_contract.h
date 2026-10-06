#pragma once
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>

#include "storage/storage.h"

namespace tideskv::test {

class StorageHarness {
 public:
  virtual ~StorageHarness() = default;
  virtual Storage& storage() = 0;
  virtual bool SupportsReopen() const = 0;
  virtual void Reopen() = 0;  // close + reopen the same data; persistent backends only
};

struct ContractParam {
  std::string name;
  std::function<std::unique_ptr<StorageHarness>()> make;
};

inline std::string ContractParamName(const ::testing::TestParamInfo<ContractParam>& info) {
  return info.param.name;
}

class StorageContractTest : public ::testing::TestWithParam<ContractParam> {
 protected:
  void SetUp() override { harness_ = GetParam().make(); }
  Storage& storage() { return harness_->storage(); }

  std::unique_ptr<StorageHarness> harness_;
};

}  // namespace tideskv::test
