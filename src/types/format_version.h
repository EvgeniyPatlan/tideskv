#pragma once
#include <string_view>

#include "common/status.h"
#include "encoding/key_layout.h"
#include "storage/storage.h"

namespace tideskv {

inline constexpr std::string_view kFormatVersionKey = "format_version";  // in the sys CF
// The M2 plan D1/D2 layout; "2" is the same without the slot in the keyspace keys (slotless layout design).
inline constexpr std::string_view kFormatVersionSlotted = "1";
inline constexpr std::string_view kFormatVersionSlotless = "2";

std::string_view FormatVersionFor(encoding::KeyLayout layout);

enum class FormatOutcome {
  kCreated,   // a fresh store: the configured layout was written
  kKept,      // the stored version is the configured one
  kSwitched,  // an empty store was rewritten to the configured layout
  kLegacy,    // a slotted store with keys in standalone mode: runs slotted, as before format 2
};

struct OpenedFormat {
  encoding::KeyLayout layout = encoding::KeyLayout::kSlotted;
  FormatOutcome outcome = FormatOutcome::kKept;
};

// Picks the key layout the store is read with, before anything else touches it. The configured layout is
// slotless with cluster mode off and slotted with it on. A fresh store gets the configured layout's version;
// an existing one keeps its own, unless it is empty: no entry in the meta, subkey, zscore or expire CF, and
// no retained replog record that carries an op (so no PSYNC can ship a key in the old layout), which is
// switched. InvalidArgument for a slotless store with keys and cluster mode on; Corruption for an unknown
// version.
Result<OpenedFormat> OpenFormat(Storage& storage, bool cluster_enabled);

}  // namespace tideskv
