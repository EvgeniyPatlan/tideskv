#include "cluster/migration_record.h"

#include <utility>

#include "common/coding.h"

namespace tideskv::cluster {
namespace {

void PutRange(std::string* out, SlotRange r) {
  coding::PutFixed16BE(out, r.start);
  coding::PutFixed16BE(out, r.end);
}

void PutEndpoint(std::string* out, const std::string& id, const std::string& host, uint16_t port) {
  out->push_back(static_cast<char>(id.size()));
  out->append(id);
  coding::PutFixed16BE(out, static_cast<uint16_t>(host.size()));
  out->append(host);
  coding::PutFixed16BE(out, port);
}

class Reader {
 public:
  explicit Reader(std::string_view in) : in_(in) {}
  bool U8(uint8_t* v) {
    if (in_.empty()) return false;
    *v = static_cast<uint8_t>(in_[0]);
    in_.remove_prefix(1);
    return true;
  }
  bool U16(uint16_t* v) { return coding::GetFixed16BE(&in_, v); }
  bool Text(size_t n, std::string* out) {
    if (in_.size() < n) return false;
    out->assign(in_.substr(0, n));
    in_.remove_prefix(n);
    return true;
  }
  bool Range(SlotRange* r) {
    return U16(&r->start) && U16(&r->end) && r->start <= r->end && r->end < kSlotCount;
  }
  bool Endpoint(std::string* id, std::string* host, uint16_t* port) {
    uint8_t id_len = 0;
    uint16_t host_len = 0;
    return U8(&id_len) && Text(id_len, id) && U16(&host_len) && Text(host_len, host) && U16(port);
  }
  bool Ranges(std::vector<SlotRange>* out) {
    uint16_t n = 0;
    if (!U16(&n)) return false;
    for (uint16_t i = 0; i < n; ++i) {
      SlotRange r;
      if (!Range(&r)) return false;
      out->push_back(r);
    }
    return true;
  }
  bool done() const { return in_.empty(); }

 private:
  std::string_view in_;
};

bool ReadJob(Reader& in, std::optional<SourceJob>* job) {
  uint8_t present = 0;
  if (!in.U8(&present) || present > 1) return false;
  if (present == 0) return true;
  SourceJob j;
  uint8_t state = 0;
  if (!in.Range(&j.range) || !in.Endpoint(&j.dst_id, &j.dst_host, &j.dst_port) || !in.U8(&state))
    return false;
  if (state != static_cast<uint8_t>(MigrateState::kSuccess) &&
      state != static_cast<uint8_t>(MigrateState::kFail)) {
    return false;  // a running job is never persisted
  }
  j.state = static_cast<MigrateState>(state);
  *job = std::move(j);
  return true;
}

bool ReadMigrated(Reader& in, std::vector<MigratedRange>* out) {
  uint16_t n = 0;
  if (!in.U16(&n)) return false;
  for (uint16_t i = 0; i < n; ++i) {
    MigratedRange m;
    if (!in.Range(&m.range) || !in.Endpoint(&m.dst_id, &m.dst_host, &m.dst_port)) return false;
    out->push_back(std::move(m));
  }
  return true;
}

bool ReadImport(Reader& in, std::optional<ImportJob>* import) {
  uint8_t present = 0;
  if (!in.U8(&present) || present > 1) return false;
  if (present == 0) return true;
  ImportJob j;
  uint8_t state = 0;
  if (!in.Range(&j.range) || !in.U8(&state) || state > static_cast<uint8_t>(ImportState::kError))
    return false;
  j.state = static_cast<ImportState>(state);
  *import = j;
  return true;
}

}  // namespace

std::string EncodeMigrationRecord(const MigrationRecord& r) {
  std::string out(1, static_cast<char>(kMigrationRecordFormat));
  out.push_back(r.job ? 1 : 0);
  if (r.job) {
    PutRange(&out, r.job->range);
    PutEndpoint(&out, r.job->dst_id, r.job->dst_host, r.job->dst_port);
    out.push_back(static_cast<char>(r.job->state));
  }
  coding::PutFixed16BE(&out, static_cast<uint16_t>(r.migrated.size()));
  for (const MigratedRange& m : r.migrated) {
    PutRange(&out, m.range);
    PutEndpoint(&out, m.dst_id, m.dst_host, m.dst_port);
  }
  coding::PutFixed16BE(&out, static_cast<uint16_t>(r.cleanup.size()));
  for (SlotRange c : r.cleanup) PutRange(&out, c);
  out.push_back(r.import ? 1 : 0);
  if (r.import) {
    PutRange(&out, r.import->range);
    out.push_back(static_cast<char>(r.import->state));
  }
  coding::PutFixed16BE(&out, static_cast<uint16_t>(r.imported.size()));
  for (SlotRange i : r.imported) PutRange(&out, i);
  return out;
}

Result<MigrationRecord> DecodeMigrationRecord(std::string_view raw) {
  Reader in(raw);
  uint8_t format = 0;
  if (!in.U8(&format) || format != kMigrationRecordFormat)
    return Status::Corruption("sys/migration: unknown format");
  MigrationRecord r;
  const bool ok = ReadJob(in, &r.job) && ReadMigrated(in, &r.migrated) && in.Ranges(&r.cleanup) &&
                  ReadImport(in, &r.import) && in.Ranges(&r.imported) && in.done();
  if (!ok) return Status::Corruption("sys/migration: damaged record");
  return r;
}

Result<std::optional<MigrationRecord>> LoadMigrationRecord(Storage& storage) {
  auto view = storage.BeginRead();
  if (!view.ok()) return view.status();
  auto raw = (*view)->Get(ColumnFamily::kSys, kSysMigration);
  if (!raw.ok()) {
    if (raw.status().IsNotFound()) return std::optional<MigrationRecord>();
    return raw.status();
  }
  auto decoded = DecodeMigrationRecord(*raw);
  if (!decoded.ok()) return decoded.status();
  return std::optional<MigrationRecord>(std::move(decoded).value());
}

encoding::ReplOp MigrationRecordPut(const MigrationRecord& r) {
  return {encoding::ReplOpType::kPut, ColumnFamily::kSys, std::string(kSysMigration),
          EncodeMigrationRecord(r)};
}

MigrationRecord RecordOf(const SlotStates& s) {
  MigrationRecord r;
  if (s.job && s.job->state != MigrateState::kStart) r.job = s.job;
  r.migrated = s.migrated;
  r.cleanup = s.cleanup;
  r.import = s.import;
  r.imported = s.imported;
  return r;
}

SlotStates StatesFromRecord(const MigrationRecord& r) {
  SlotStates s;
  s.job = r.job;
  s.migrated = r.migrated;
  s.cleanup = r.cleanup;
  s.import = r.import;
  s.imported = r.imported;
  for (const MigratedRange& m : s.migrated) SetFenced(s, m.range, true);
  for (SlotRange c : s.cleanup) SetFenced(s, c, true);
  return s;
}

}  // namespace tideskv::cluster
