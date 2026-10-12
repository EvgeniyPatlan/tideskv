#include "types/lcs.h"

#include <algorithm>

#include "types/errors.h"

namespace tideskv {
namespace {

class DpTable {
 public:
  DpTable(std::string_view a, std::string_view b) : cols_(b.size() + 1), cells_((a.size() + 1) * cols_, 0) {
    for (size_t i = 1; i <= a.size(); ++i) {
      for (size_t j = 1; j <= b.size(); ++j) {
        at(i, j) = a[i - 1] == b[j - 1] ? at(i - 1, j - 1) + 1 : std::max(at(i - 1, j), at(i, j - 1));
      }
    }
  }
  uint32_t at(size_t i, size_t j) const { return cells_[i * cols_ + j]; }

 private:
  uint32_t& at(size_t i, size_t j) { return cells_[i * cols_ + j]; }
  size_t cols_;
  std::vector<uint32_t> cells_;
};

// Walks the table back from (alen, blen), filling the sequence and the match ranges.
void WalkBack(const DpTable& dp, std::string_view a, std::string_view b, int64_t min_len, LcsResult* out) {
  const auto alen = static_cast<uint32_t>(a.size());
  uint32_t idx = out->length, i = alen, j = static_cast<uint32_t>(b.size());
  uint32_t as = alen, ae = 0, bs = 0, be = 0;  // as == alen: no current range
  out->sequence.assign(out->length, '\0');
  while (i > 0 && j > 0) {
    bool emit = false;
    if (a[i - 1] == b[j - 1]) {
      out->sequence[idx - 1] = a[i - 1];
      if (as == alen) {
        as = ae = i - 1;
        bs = be = j - 1;
      } else if (as == i && bs == j) {
        --as;
        --bs;
      } else {
        emit = true;
      }
      if (as == 0 || bs == 0) emit = true;
      --idx;
      --i;
      --j;
    } else {
      dp.at(i - 1, j) > dp.at(i, j - 1) ? --i : --j;
      if (as != alen) emit = true;
    }
    const uint32_t len = ae - as + 1;
    if (emit) {
      if (min_len == 0 || static_cast<int64_t>(len) >= min_len) out->matches.push_back({as, ae, bs, be, len});
      as = alen;
    }
  }
}

}  // namespace

Result<LcsResult> ComputeLcs(std::string_view a, std::string_view b, bool compute_sequence,
                             int64_t min_match_len) {
  // Redis sends this same text when its DP-table allocation fails; we refuse up front instead.
  const unsigned long long cells = (a.size() + 1ULL) * (b.size() + 1ULL);
  if (cells > kMaxStringSize / sizeof(uint32_t)) {
    return Status::UserError(std::string(kErrLcsMemory));
  }
  const DpTable dp(a, b);
  LcsResult out;
  out.length = dp.at(a.size(), b.size());
  if (compute_sequence) WalkBack(dp, a, b, min_match_len < 0 ? 0 : min_match_len, &out);
  return out;
}

}  // namespace tideskv
