#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "commands/registry.h"
#include "common/status.h"

// Redis 7.2 mpop argument parsing (lmpopGenericCommand / zmpopGenericCommand):
//   ... numkeys key [key ...] FIRST|SECOND [COUNT count]
namespace tideskv {

struct MultiPopArgs {
  size_t first_key = 0;  // argv index of the first key
  size_t num_keys = 0;
  bool second = false;  // the second word (RIGHT / MAX) was given
  int64_t count = 1;
  bool has_count = false;
};

// numkeys: kErrNumkeysNotPositive for anything but an integer >= 1; too few arguments for the keys
// plus the direction word, an unknown word, a second COUNT or junk: kErrSyntax; a COUNT value that
// is not an integer >= 1: kErrCountGreaterThanZero.
Status ParseMultiPop(const ArgList& argv, size_t numkeys_idx, std::string_view first_word,
                     std::string_view second_word, MultiPopArgs* out);

}  // namespace tideskv
