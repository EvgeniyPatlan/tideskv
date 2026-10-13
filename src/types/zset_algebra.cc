#include "types/zset_algebra.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "types/collection.h"
#include "types/errors.h"
#include "types/object_store.h"

namespace tideskv {
namespace {

using encoding::ValueType;

void Aggregate1(double* total, double v, Aggregate a) {
  switch (a) {
    case Aggregate::kSum:
      *total += v;
      if (std::isnan(*total)) *total = 0.0;
      return;
    case Aggregate::kMin:
      *total = v < *total ? v : *total;
      return;
    case Aggregate::kMax:
      *total = v > *total ? v : *total;
      return;
  }
}

// Source indexes in ascending cardinality; stable, like glibc's merge-sort qsort.
std::vector<size_t> BySize(const std::vector<ZsetSource>& sources) {
  std::vector<size_t> order(sources.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return sources[a].size() < sources[b].size(); });
  return order;
}

double Clean(double v) { return std::isnan(v) ? 0.0 : v; }

std::vector<ScoreMember> Sorted(std::map<std::string, double, std::less<>>&& acc) {
  std::vector<ScoreMember> out;
  out.reserve(acc.size());
  for (auto& [m, s] : acc) out.push_back({s, m});
  std::stable_sort(out.begin(), out.end(), [](const ScoreMember& a, const ScoreMember& b) {
    return a.score < b.score;  // ties keep member byte order (the map's order)
  });
  return out;
}

std::vector<ScoreMember> Union(const std::vector<ZsetSource>& src, const std::vector<double>& w,
                               Aggregate a) {
  std::map<std::string, double, std::less<>> acc;
  for (size_t i : BySize(src)) {
    for (const auto& [m, s] : src[i]) {
      const double v = Clean(w[i] * s);
      auto [it, fresh] = acc.try_emplace(m, v);
      if (!fresh) Aggregate1(&it->second, v, a);
    }
  }
  return Sorted(std::move(acc));
}

std::vector<ScoreMember> Inter(const std::vector<ZsetSource>& src, const std::vector<double>& w,
                               Aggregate a) {
  const std::vector<size_t> order = BySize(src);
  std::map<std::string, double, std::less<>> acc;
  const ZsetSource& smallest = src[order.front()];
  for (const auto& [m, s] : smallest) {
    double total = Clean(w[order.front()] * s);
    bool everywhere = true;
    for (size_t k = 1; k < order.size() && everywhere; ++k) {
      const auto it = src[order[k]].find(m);
      if (it == src[order[k]].end()) {
        everywhere = false;
      } else {
        Aggregate1(&total, w[order[k]] * it->second, a);
      }
    }
    if (everywhere) acc.emplace(m, total);
  }
  return Sorted(std::move(acc));
}

std::vector<ScoreMember> Diff(const std::vector<ZsetSource>& src, bool first_key_repeated) {
  std::map<std::string, double, std::less<>> acc;
  if (first_key_repeated) return {};
  for (const auto& [m, s] : src.front()) {
    const bool elsewhere =
        std::any_of(src.begin() + 1, src.end(), [&](const ZsetSource& o) { return o.contains(m); });
    if (!elsewhere) acc.emplace(m, s);
  }
  return Sorted(std::move(acc));
}

}  // namespace

Result<std::vector<ZsetSource>> LoadZsetSources(ReadView& view, uint8_t db,
                                                const std::vector<std::string_view>& keys, int64_t now_ms) {
  std::vector<ZsetSource> out;
  out.reserve(keys.size());
  for (std::string_view key : keys) {
    auto obj = LoadObject(view, db, key, now_ms);
    if (!obj.ok()) return obj.status();
    ZsetSource src;
    if (obj->live()) {
      const ValueType type = obj->meta.type;
      if (type != ValueType::kZSet && type != ValueType::kSet)
        return Status::UserError(std::string(kErrWrongType));
      auto c = LoadCollection(view, db, key, type, now_ms);
      if (!c.ok()) return c.status();
      bool bad = false;
      Status st = ForEachElement(view, *c, [&](std::string_view m, std::string_view v) {
        if (type == ValueType::kSet) {
          src.emplace(std::string(m), 1.0);
          return true;
        }
        if (v.size() != 8) {  // a zset score is always 8 bytes
          bad = true;
          return false;
        }
        src.emplace(std::string(m), DecodeRawScore(v));
        return true;
      });
      if (!st.ok()) return st;
      if (bad) return Status::Corruption("bad zset score");
    }
    out.push_back(std::move(src));
  }
  return out;
}

std::vector<ScoreMember> CombineZsets(ZsetOp op, const std::vector<ZsetSource>& sources,
                                      const std::vector<double>& weights, Aggregate aggregate,
                                      bool first_key_repeated) {
  if (sources.empty()) return {};
  switch (op) {
    case ZsetOp::kUnion:
      return Union(sources, weights, aggregate);
    case ZsetOp::kInter:
      return Inter(sources, weights, aggregate);
    case ZsetOp::kDiff:
      return Diff(sources, first_key_repeated);
  }
  return {};
}

uint64_t ZsetInterCardinality(const std::vector<ZsetSource>& sources, uint64_t limit) {
  if (sources.empty()) return 0;
  const std::vector<size_t> order = BySize(sources);
  uint64_t n = 0;
  for (const auto& [m, s] : sources[order.front()]) {
    const bool everywhere =
        std::all_of(order.begin() + 1, order.end(), [&](size_t i) { return sources[i].contains(m); });
    if (everywhere && ++n == limit) break;
  }
  return n;
}

}  // namespace tideskv
