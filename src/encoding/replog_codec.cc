#include "encoding/replog_codec.h"

#include "common/coding.h"
#include "common/crc32c.h"

namespace tideskv::encoding {
namespace {

constexpr size_t kCrcSize = 4;
constexpr size_t kMinOpSize = 4;  // type, cf, empty key length, empty value length
constexpr size_t kCumBytesOffset = 1 + 8;

Status Bad(const char* reason) { return Status::Corruption(std::string("replog batch: ") + reason); }

size_t VarintSize(uint64_t v) {
  size_t n = 1;
  for (; v >= 0x80; v >>= 7) ++n;
  return n;
}

void PutField(std::string* dst, const std::string& field) {
  coding::PutVarint64(dst, field.size());
  dst->append(field);
}

// Reads a length-prefixed field; rejects non-minimal varints and lengths past the end.
bool GetField(std::string_view* in, std::string* out) {
  const std::string_view before = *in;
  uint64_t len = 0;
  if (!coding::GetVarint64(in, &len)) return false;
  if (before.size() - in->size() != VarintSize(len)) return false;
  if (len > in->size()) return false;
  out->assign(in->data(), len);
  in->remove_prefix(len);
  return true;
}

bool ValidType(uint8_t t) {
  return t >= static_cast<uint8_t>(ReplOpType::kPut) && t <= static_cast<uint8_t>(ReplOpType::kDeleteRange);
}

bool ValidCf(uint8_t cf) {
  return cf < kColumnFamilyCount && cf != static_cast<uint8_t>(ColumnFamily::kReplog);
}

Status DecodeOp(std::string_view* in, ReplOp* op) {
  if (in->size() < kMinOpSize) return Bad("truncated op");
  const auto type = static_cast<uint8_t>((*in)[0]);
  const auto cf = static_cast<uint8_t>((*in)[1]);
  if (!ValidType(type)) return Bad("bad op type");
  if (!ValidCf(cf)) return Bad("bad column family");
  in->remove_prefix(2);
  op->type = static_cast<ReplOpType>(type);
  op->cf = static_cast<ColumnFamily>(cf);
  if (!GetField(in, &op->key)) return Bad("bad key field");
  if (!GetField(in, &op->value)) return Bad("bad value field");
  return Status::Ok();
}

}  // namespace

std::string ReplogKey(uint64_t seq) {
  std::string key;
  key.reserve(8);
  coding::PutFixed64BE(&key, seq);
  return key;
}

bool DecodeReplogKey(std::string_view key, uint64_t* seq) {
  if (key.size() != 8) return false;
  *seq = coding::DecodeFixed64BE(key.data());
  return true;
}

std::string EncodeReplogBatch(const std::vector<ReplOp>& ops, int64_t timestamp_ms,
                              uint64_t cum_bytes_before) {
  std::string out;
  out.push_back(static_cast<char>(kReplogVersion));
  coding::PutFixed64BE(&out, static_cast<uint64_t>(timestamp_ms));
  coding::PutFixed64BE(&out, 0);  // cum_bytes placeholder, patched once the size is known
  coding::PutFixed32BE(&out, static_cast<uint32_t>(ops.size()));
  for (const ReplOp& op : ops) {
    out.push_back(static_cast<char>(op.type));
    out.push_back(static_cast<char>(op.cf));
    PutField(&out, op.key);
    PutField(&out, op.value);
  }
  std::string cum;
  coding::PutFixed64BE(&cum, cum_bytes_before + out.size() + kCrcSize);
  out.replace(kCumBytesOffset, 8, cum);
  coding::PutFixed32BE(&out, Crc32c(out));
  return out;
}

size_t ReplogBatchSize(const std::vector<ReplOp>& ops) {
  size_t size = kReplogHeaderSize + kCrcSize;
  for (const ReplOp& op : ops) {
    size += 2 + VarintSize(op.key.size()) + op.key.size() + VarintSize(op.value.size()) + op.value.size();
  }
  return size;
}

bool PeekReplogHeader(std::string_view encoded, int64_t* timestamp_ms, uint64_t* cum_bytes) {
  if (encoded.size() < kReplogHeaderSize) return false;
  *timestamp_ms = static_cast<int64_t>(coding::DecodeFixed64BE(encoded.data() + 1));
  *cum_bytes = coding::DecodeFixed64BE(encoded.data() + kCumBytesOffset);
  return true;
}

Result<ReplogBatch> DecodeReplogBatch(std::string_view encoded) {
  if (encoded.size() < kReplogHeaderSize + kCrcSize) return Bad("too short");
  const std::string_view body = encoded.substr(0, encoded.size() - kCrcSize);
  if (coding::DecodeFixed32BE(encoded.data() + body.size()) != Crc32c(body)) return Bad("checksum mismatch");
  if (static_cast<uint8_t>(body[0]) != kReplogVersion) return Bad("unsupported version");

  ReplogBatch batch;
  PeekReplogHeader(body, &batch.timestamp_ms, &batch.cum_bytes);
  const uint32_t op_count = coding::DecodeFixed32BE(body.data() + kReplogHeaderSize - 4);
  std::string_view rest = body.substr(kReplogHeaderSize);
  if (op_count > rest.size() / kMinOpSize) return Bad("op count exceeds payload");
  batch.ops.resize(op_count);
  for (ReplOp& op : batch.ops) {
    if (Status s = DecodeOp(&rest, &op); !s.ok()) return s;
  }
  if (!rest.empty()) return Bad("trailing bytes");
  return batch;
}

}  // namespace tideskv::encoding
