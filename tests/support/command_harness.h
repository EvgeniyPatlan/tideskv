#pragma once
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>

#include "commands/all_commands.h"
#include "common/coding.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "support/fake_server_services.h"
#include "types/object_store.h"

namespace tideskv::test {

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
