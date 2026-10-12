#include "types/scan_position.h"

#include <random>

namespace tideskv {
namespace {

uint64_t LoadLe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

uint64_t Rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

struct SipState {
  uint64_t v0, v1, v2, v3;

  void Round() {
    v0 += v1;
    v1 = Rotl(v1, 13);
    v1 ^= v0;
    v0 = Rotl(v0, 32);
    v2 += v3;
    v3 = Rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = Rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = Rotl(v1, 17);
    v1 ^= v2;
    v2 = Rotl(v2, 32);
  }
  void Compress(uint64_t m) {
    v3 ^= m;
    Round();
    Round();
    v0 ^= m;
  }
};

SipHashKey RandomKey() {
  std::random_device rd;
  SipHashKey key{};
  for (size_t i = 0; i < key.size(); i += 4) {
    const uint32_t r = rd();
    for (size_t j = 0; j < 4; ++j) key[i + j] = static_cast<uint8_t>(r >> (8 * j));
  }
  return key;
}

}  // namespace

uint64_t SipHash24(const SipHashKey& key, std::string_view s) {
  const uint64_t k0 = LoadLe64(key.data());
  const uint64_t k1 = LoadLe64(key.data() + 8);
  SipState st{k0 ^ 0x736f6d6570736575ULL, k1 ^ 0x646f72616e646f6dULL, k0 ^ 0x6c7967656e657261ULL,
              k1 ^ 0x7465646279746573ULL};
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  const size_t full = s.size() / 8 * 8;
  for (size_t i = 0; i < full; i += 8) st.Compress(LoadLe64(p + i));
  uint64_t last = static_cast<uint64_t>(s.size() & 0xff) << 56;
  for (size_t i = full; i < s.size(); ++i) last |= static_cast<uint64_t>(p[i]) << (8 * (i - full));
  st.Compress(last);
  st.v2 ^= 0xff;
  for (int i = 0; i < 4; ++i) st.Round();
  return st.v0 ^ st.v1 ^ st.v2 ^ st.v3;
}

uint64_t PositionDigest(std::string_view s) {
  static const SipHashKey kKey = RandomKey();  // thread-safe one-time init (C++11 magic statics)
  return SipHash24(kKey, s);
}

}  // namespace tideskv
