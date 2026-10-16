#include "commands/key_extraction.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "common/string_util.h"

namespace tideskv {
namespace {

using Extractor = std::vector<size_t> (*)(const ArgList&);

// numkeys at argv[count_at], keys from argv[first] (db.c genericGetKeys).
std::vector<size_t> NumKeys(const ArgList& argv, size_t count_at, size_t first) {
  int64_t n = 0;
  if (count_at >= argv.size() || !ParseInt64(argv[count_at], &n) || n < 1) return {};
  if (first > argv.size() || static_cast<uint64_t>(n) > argv.size() - first) return {};
  std::vector<size_t> out(static_cast<size_t>(n));
  for (size_t i = 0; i < out.size(); ++i) out[i] = first + i;
  return out;
}

std::vector<size_t> NumKeysAt1(const ArgList& a) { return NumKeys(a, 1, 2); }  // SINTERCARD ... ZMPOP
std::vector<size_t> NumKeysAt2(const ArgList& a) { return NumKeys(a, 2, 3); }  // BLMPOP BZMPOP EVAL*

std::vector<size_t> StoreKeys(const ArgList& a) {  // zunionInterDiffStoreGetKeys: destination + sources
  std::vector<size_t> keys = NumKeys(a, 2, 3);
  if (!keys.empty()) keys.insert(keys.begin(), 1);
  return keys;
}

const std::unordered_map<std::string_view, Extractor>& Movable() {
  static const std::unordered_map<std::string_view, Extractor> kTable = {
      {"sintercard", &NumKeysAt1}, {"zintercard", &NumKeysAt1}, {"zunion", &NumKeysAt1},
      {"zinter", &NumKeysAt1},     {"zdiff", &NumKeysAt1},      {"lmpop", &NumKeysAt1},
      {"zmpop", &NumKeysAt1},      {"blmpop", &NumKeysAt2},     {"bzmpop", &NumKeysAt2},
      {"eval", &NumKeysAt2},       {"evalsha", &NumKeysAt2},    {"eval_ro", &NumKeysAt2},
      {"evalsha_ro", &NumKeysAt2}, {"zunionstore", &StoreKeys}, {"zinterstore", &StoreKeys},
      {"zdiffstore", &StoreKeys}};
  return kTable;
}

std::vector<size_t> TripleKeys(const CommandSpec& s, const ArgList& argv) {
  if (s.first_key <= 0) return {};
  const auto argc = static_cast<int64_t>(argv.size());
  const int64_t last = s.last_key < 0 ? argc + s.last_key : s.last_key;
  const int64_t step = std::max(1, s.key_step);
  std::vector<size_t> out;
  for (int64_t i = s.first_key; i <= last && i < argc; i += step) out.push_back(static_cast<size_t>(i));
  return out;
}

}  // namespace

std::vector<size_t> KeyIndexes(const CommandSpec& spec, const ArgList& argv) {
  const auto& table = Movable();
  if (const auto it = table.find(spec.name); it != table.end()) return it->second(argv);
  return TripleKeys(spec, argv);
}

bool HasMovableKeys(std::string_view name) { return Movable().count(name) == 1; }

bool NoMandatoryKeys(std::string_view name) {
  return name == "eval" || name == "evalsha" || name == "eval_ro" || name == "evalsha_ro";
}

}  // namespace tideskv
