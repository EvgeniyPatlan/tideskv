// The unit suite's main: TIDESKV_TEST_KEY_LAYOUT=slotless runs every test with the slotless key layout (ctest
// registers the suite a second time with it, prefix "slotless."). Tests that spell slotted bytes pin the
// layout with encoding::ScopedKeyLayout; cluster and migration tests skip (support/key_layout_skip.h).
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "encoding/key_layout.h"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (const char* env = std::getenv("TIDESKV_TEST_KEY_LAYOUT"); env != nullptr && *env != '\0') {
    const auto layout = tideskv::encoding::ParseKeyLayout(env);
    if (!layout) {
      std::fprintf(stderr, "TIDESKV_TEST_KEY_LAYOUT must be slotted or slotless, not '%s'\n", env);
      return 2;
    }
    tideskv::encoding::AdoptKeyLayout(*layout);  // not InitKeyLayout: KeyLayout.InitIsRefusedTheSecondTime
  }
  return RUN_ALL_TESTS();
}
