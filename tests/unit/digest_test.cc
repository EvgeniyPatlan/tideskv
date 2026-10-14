#include "types/digest.h"

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/score_format.h"
#include "common/sha1.h"
#include "exec/engine.h"
#include "support/command_harness.h"

namespace tideskv {
namespace {

using test::CommandHarness;

TEST(Sha1Raw, MatchesTheHexForm) {
  const Digest20 d = Sha1("abc");
  EXPECT_EQ(DigestHex(d), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(DigestHex(Sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(DigestHex(Digest20{}), std::string(40, '0'));
}

// ---- A test-local model of Redis's computeDatasetDigest (debug.c), from the same primitives. ----

void Mix(Digest20& d, std::string_view bytes) {  // d = SHA1(d ^ SHA1(bytes))
  const Digest20 h = Sha1(bytes);
  for (size_t i = 0; i < d.size(); ++i) d[i] ^= h[i];
  d = Sha1(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
}
void Xor(Digest20& d, std::string_view bytes) {
  const Digest20 h = Sha1(bytes);
  for (size_t i = 0; i < d.size(); ++i) d[i] ^= h[i];
}
std::string_view Bytes(const Digest20& d) { return {reinterpret_cast<const char*>(d.data()), d.size()}; }
std::string BigEndian32(uint32_t v) {
  return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
          static_cast<char>(v)};
}

const std::vector<std::string> kScores = {
    "1.5", "2", "-3", "0.1", "1e16", "1e20", "3e-7", "123456789012345678", "inf", "-inf", "0"};

struct ModelKey {
  int type = 0;                    // Redis type code: 0 string, 1 list, 2 set, 3 zset, 4 hash
  std::vector<std::string> items;  // string: [value]; list: elements; set: members; zset/hash: pairs
  bool ttl = false;
};

// xorObjectDigest: continues mixing into d.
Digest20 ModelObject(Digest20 d, const ModelKey& k) {
  Mix(d, BigEndian32(static_cast<uint32_t>(k.type)));
  if (k.type == 0 || k.type == 1) {
    for (const auto& e : k.items) Mix(d, e);
  } else if (k.type == 2) {
    for (const auto& m : std::set<std::string>(k.items.begin(), k.items.end())) Xor(d, m);
  } else {
    std::map<std::string, std::string> pairs;  // items are (score|value, member|field); last write wins
    for (size_t i = 0; i + 1 < k.items.size(); i += 2) pairs[k.items[i + 1]] = k.items[i];
    for (const auto& [name, val] : pairs) {
      Digest20 e{};
      Mix(e, name);
      Mix(e, k.type == 3 ? FormatDtoa(std::strtod(val.c_str(), nullptr)) : val);
      Xor(d, Bytes(e));
    }
  }
  if (k.ttl) Xor(d, "!!expire!!");
  return d;
}

using Model = std::map<uint8_t, std::map<std::string, ModelKey>>;

Digest20 ModelDataset(const Model& model) {
  Digest20 final_digest{};
  for (const auto& [db, keys] : model) {
    if (keys.empty()) continue;
    Mix(final_digest, BigEndian32(db));
    for (const auto& [name, k] : keys) {
      Digest20 d{};
      Mix(d, name);
      Xor(final_digest, Bytes(ModelObject(d, k)));
    }
  }
  return final_digest;
}

std::vector<std::string> WriteCommand(const std::string& key, const ModelKey& k) {
  static const char* kCmd[] = {"SET", "RPUSH", "SADD", "ZADD", "HSET"};
  std::vector<std::string> argv = {kCmd[k.type], key};
  argv.insert(argv.end(), k.items.begin(), k.items.end());
  return argv;
}

rc::Gen<std::string> GenText() { return rc::gen::container<std::string>(rc::gen::inRange<char>(0, 127)); }

// The argument list after the key: string [value]; list/set elements; zset/hash (score|value, name) pairs.
rc::Gen<std::vector<std::string>> GenItems(int type) {
  if (type == 0)
    return rc::gen::map(GenText(), [](std::string s) { return std::vector<std::string>{std::move(s)}; });
  if (type == 1 || type == 2) {
    return rc::gen::nonEmpty(rc::gen::container<std::vector<std::string>>(rc::gen::resize(5, GenText())));
  }
  auto first = type == 3 ? rc::gen::elementOf(kScores) : GenText();
  auto pairs = rc::gen::nonEmpty(rc::gen::container<std::vector<std::pair<std::string, std::string>>>(
      rc::gen::resize(4, rc::gen::pair(first, GenText()))));
  return rc::gen::map(pairs, [](const std::vector<std::pair<std::string, std::string>>& ps) {
    std::vector<std::string> out;
    for (const auto& [a, b] : ps) {
      out.push_back(a);
      out.push_back(b);
    }
    return out;
  });
}

rc::Gen<ModelKey> GenKey() {
  return rc::gen::mapcat(rc::gen::inRange(0, 5), [](int type) {
    return rc::gen::build<ModelKey>(rc::gen::set(&ModelKey::type, rc::gen::just(type)),
                                    rc::gen::set(&ModelKey::items, GenItems(type)),
                                    rc::gen::set(&ModelKey::ttl, rc::gen::arbitrary<bool>()));
  });
}

RC_GTEST_PROP(DigestProp, DatasetDigestMatchesTheRedisModel, ()) {
  const auto entries = *rc::gen::container<std::vector<std::pair<int, ModelKey>>>(
      rc::gen::resize(6, rc::gen::pair(rc::gen::inRange(0, 9), GenKey())));
  CommandHarness h;
  Model model;
  for (const auto& [slot, k] : entries) {
    const uint8_t db = static_cast<uint8_t>(slot % 3 == 0 ? 0 : (slot % 3 == 1 ? 1 : 15));
    const std::string name = "k" + std::to_string(slot);
    if (model[db].count(name) != 0) continue;  // one write per key keeps the model simple
    model[db][name] = k;
    h.Run({"SELECT", std::to_string(db)});
    RC_ASSERT(h.Run(WriteCommand(name, k)).substr(0, 1) != "-");
    if (k.ttl) RC_ASSERT(h.Run({"PEXPIREAT", name, "4102444800000"}) == test::Int(1));
  }
  Digest20 got{};
  Digest20 got_value{};
  Status st = h.services.engine().Snapshot([&](ReadView& v, int64_t) {
    auto r = DatasetDigest(v);
    if (!r.ok()) return r.status();
    got = *r;
    if (!model.empty() && !model.begin()->second.empty()) {
      const auto& [db, keys] = *model.begin();
      auto rv = ValueDigest(v, db, keys.begin()->first);
      if (!rv.ok()) return rv.status();
      got_value = *rv;
    }
    return Status::Ok();
  });
  RC_ASSERT(st.ok());
  RC_ASSERT(got == ModelDataset(model));
  if (!model.empty() && !model.begin()->second.empty()) {
    RC_ASSERT(got_value == ModelObject(Digest20{}, model.begin()->second.begin()->second));
  }
}

TEST(Digest, ValueDigestOfMissingKeyIsZeros) {
  CommandHarness h;
  Digest20 got{1};
  ASSERT_TRUE(h.services.engine()
                  .Snapshot([&](ReadView& v, int64_t) {
                    auto r = ValueDigest(v, 0, "nokey");
                    if (r.ok()) got = *r;
                    return r.status();
                  })
                  .ok());
  EXPECT_EQ(got, Digest20{});
}

}  // namespace
}  // namespace tideskv
