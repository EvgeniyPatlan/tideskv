#pragma once
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#include "commands/all_commands.h"
#include "common/coding.h"
#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "encoding/object_codec.h"
#include "support/fake_server_services.h"
#include "types/object_store.h"

namespace tideskv::test {

// Expected RESP2 reply bytes.
inline const std::string kOk = "+OK\r\n";
inline const std::string kNil = "$-1\r\n";
inline std::string Bulk(std::string_view s) {
  return "$" + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n";
}
inline std::string Int(int64_t n) { return ":" + std::to_string(n) + "\r\n"; }
inline std::string Err(std::string_view msg) { return "-" + std::string(msg) + "\r\n"; }
inline std::string Arr(std::initializer_list<std::string> items) {
  std::string out = "*" + std::to_string(items.size()) + "\r\n";
  for (const auto& i : items) out += i;
  return out;
}

// Runs commands through the real registry + Dispatch, returning raw reply bytes.
struct CommandHarness {
  CommandHarness() {
    RegisterAllCommands(services.registry_);
    session.id = 7;
    session.authenticated = true;
  }

  std::string Run(const ArgList& argv) {
    ExecContext ctx{session, reply, services};
    Dispatch(services.registry(), ctx, argv);
    return reply.Take();
  }
  // Binary-safe: each element keeps its length (std::string arguments may hold NULs).
  std::string Run(std::initializer_list<std::string_view> argv) {
    return Run(ArgList(argv.begin(), argv.end()));
  }

  void RequirePassword(const std::string& pass) {
    EXPECT_TRUE(services.config_.Set("requirepass", pass, true).ok());
    session.authenticated = false;
  }

  FakeClock& clock() { return services.clock_; }

  // Writes a raw metadata record (and its expire entry), e.g. a hash, so WRONGTYPE paths
  // are testable before M3 has hash commands.
  void SeedMeta(uint8_t db, const std::string& key, const encoding::Metadata& meta,
                std::string_view payload = {}) {
    auto txn = services.storage_.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE(
        (*txn)
            ->Put(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key), encoding::EncodeMetadata(meta, payload))
            .ok());
    if (meta.expire_ms != 0) {
      ASSERT_TRUE((*txn)
                      ->Put(ColumnFamily::kExpire,
                            encoding::ExpireKeyFor(db, static_cast<uint64_t>(meta.expire_ms), key),
                            ExpireValue(meta.oid))
                      .ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }

  // A hash metadata record with object id `oid`; with_field also stores one subkey field, so
  // tests can assert that overwriting/deleting the key drops its collection data.
  void SeedHash(uint8_t db, const std::string& key, uint64_t oid, bool with_field) {
    encoding::Metadata meta;
    meta.type = encoding::ValueType::kHash;
    meta.oid = oid;
    meta.size = with_field ? 1 : 0;
    SeedMeta(db, key, meta);
    if (!with_field) return;
    auto txn = services.storage_.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE(
        (*txn)
            ->Put(ColumnFamily::kSubkey, encoding::EncodeSubkey(db, KeySlot(key), oid, "field"), "value")
            .ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }

  // The stored absolute deadline of (db, key): -1 when it has none, -2 when the key has no
  // meta record. Reads raw storage, so an expired-but-unreaped key still reports its deadline.
  int64_t ExpireOf(uint8_t db, std::string_view key) {
    const auto& meta = Cf(ColumnFamily::kMeta);
    auto it = meta.find(encoding::MetaKeyFor(db, key));
    if (it == meta.end()) return -2;
    auto decoded = encoding::DecodeMetadata(it->second);
    EXPECT_TRUE(decoded.ok());
    if (!decoded.ok()) return -2;
    return decoded->meta.expire_ms == 0 ? -1 : decoded->meta.expire_ms;
  }

  // Raw CF contents, for asserting on index entries.
  const InMemoryStorage::Table& Cf(ColumnFamily cf) {
    snapshot_ = services.storage_.Current();
    return (*snapshot_)[CfIndex(cf)];
  }

  FakeServerServices services;
  Session session;
  resp::ReplyBuilder reply;
  std::shared_ptr<const InMemoryStorage::Tables> snapshot_;
};

}  // namespace tideskv::test
