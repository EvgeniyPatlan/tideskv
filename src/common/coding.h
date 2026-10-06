#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Big-endian fixed-width integers (byte order == numeric order) and LEB128 varints.
namespace tideskv::coding {

inline void PutFixed16BE(std::string* dst, uint16_t v) {
  dst->push_back(static_cast<char>(v >> 8));
  dst->push_back(static_cast<char>(v));
}

inline void PutFixed32BE(std::string* dst, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) dst->push_back(static_cast<char>(v >> shift));
}

inline void PutFixed64BE(std::string* dst, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) dst->push_back(static_cast<char>(v >> shift));
}

inline uint16_t DecodeFixed16BE(const char* p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  return static_cast<uint16_t>((u[0] << 8) | u[1]);
}

inline uint32_t DecodeFixed32BE(const char* p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | u[i];
  return v;
}

inline uint64_t DecodeFixed64BE(const char* p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | u[i];
  return v;
}

inline bool GetFixed16BE(std::string_view* in, uint16_t* v) {
  if (in->size() < 2) return false;
  *v = DecodeFixed16BE(in->data());
  in->remove_prefix(2);
  return true;
}

inline bool GetFixed64BE(std::string_view* in, uint64_t* v) {
  if (in->size() < 8) return false;
  *v = DecodeFixed64BE(in->data());
  in->remove_prefix(8);
  return true;
}

inline void PutVarint64(std::string* dst, uint64_t v) {
  while (v >= 0x80) {
    dst->push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline bool GetVarint64(std::string_view* in, uint64_t* v) {
  uint64_t result = 0;
  for (size_t i = 0; i < in->size() && i < 10; ++i) {
    const auto byte = static_cast<unsigned char>((*in)[i]);
    result |= static_cast<uint64_t>(byte & 0x7F) << (7 * i);
    if ((byte & 0x80) == 0) {
      *v = result;
      in->remove_prefix(i + 1);
      return true;
    }
  }
  return false;  // truncated, or longer than 10 bytes
}

}  // namespace tideskv::coding
