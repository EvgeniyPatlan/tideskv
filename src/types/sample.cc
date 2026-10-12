#include "types/sample.h"

#include <algorithm>
#include <unordered_set>

namespace tideskv {

std::vector<uint64_t> SamplePositions(uint64_t n, uint64_t count, bool distinct, std::mt19937_64& rng) {
  std::vector<uint64_t> out;
  if (n == 0 || count == 0) return out;
  if (!distinct) {
    std::uniform_int_distribution<uint64_t> any(0, n - 1);
    out.reserve(count);
    for (uint64_t i = 0; i < count; ++i) out.push_back(any(rng));
  } else if (count >= n) {
    out.resize(n);
    for (uint64_t i = 0; i < n; ++i) out[i] = i;
  } else {
    std::unordered_set<uint64_t> chosen;  // Floyd: for j in [n-count, n): pick t in [0, j]
    chosen.reserve(count);
    for (uint64_t j = n - count; j < n; ++j) {
      const uint64_t t = std::uniform_int_distribution<uint64_t>(0, j)(rng);
      chosen.insert(chosen.contains(t) ? j : t);
    }
    out.assign(chosen.begin(), chosen.end());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace tideskv
