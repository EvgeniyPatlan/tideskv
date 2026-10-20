#pragma once
#include <gtest/gtest.h>

#include "encoding/key_layout.h"

// Cluster mode, and so migration, slot erase and the slot fences, exist only with the slotted layout (a
// slotless data dir refuses cluster-enabled yes). Their tests skip in the unit suite's slotless pass.
#define TIDESKV_SKIP_IF_SLOTLESS()                                       \
  do {                                                                   \
    if (::tideskv::encoding::Slotless()) {                               \
      GTEST_SKIP() << "cluster mode always uses the slotted key layout"; \
    }                                                                    \
  } while (0)
