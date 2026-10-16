#include "cluster/cluster_store.h"

#include <utility>

#include "cluster/nodes_parser.h"
#include "common/coding.h"

namespace tideskv::cluster {

namespace {
constexpr size_t kHeader = 1 + 8 + 1;  // format, version, myid length
}  // namespace

std::string EncodeClusterCfg(const Topology& t) {
  std::string out(1, static_cast<char>(kClusterCfgFormat));
  coding::PutFixed64BE(&out, static_cast<uint64_t>(t.version));
  out.push_back(static_cast<char>(t.myid.size()));
  out += t.myid;
  if (t.initialized()) out += SerializeNodes(t);
  return out;
}

Result<std::shared_ptr<Topology>> DecodeClusterCfg(std::string_view raw) {
  if (raw.size() < kHeader || static_cast<uint8_t>(raw[0]) != kClusterCfgFormat) {
    return Status::Corruption("sys/cluster_cfg: unknown format");
  }
  auto t = std::make_shared<Topology>();
  t->version = static_cast<int64_t>(coding::DecodeFixed64BE(raw.data() + 1));
  const size_t id_len = static_cast<uint8_t>(raw[9]);
  if (t->version < kNoVersion || raw.size() < kHeader + id_len)
    return Status::Corruption("sys/cluster_cfg: truncated");
  t->myid = std::string(raw.substr(kHeader, id_len));
  const std::string_view nodes = raw.substr(kHeader + id_len);
  if (t->initialized()) {
    auto parsed = ParseNodes(nodes);
    if (!parsed.ok()) return Status::Corruption("sys/cluster_cfg: " + parsed.status().message());
    t->nodes = std::move(parsed->nodes);
    t->owner = parsed->owner;
  } else if (!nodes.empty()) {
    return Status::Corruption("sys/cluster_cfg: nodes without a version");
  }
  t->ResolveMyself();
  return t;
}

Result<std::shared_ptr<Topology>> LoadClusterCfg(Storage& storage) {
  auto view = storage.BeginRead();
  if (!view.ok()) return view.status();
  auto raw = (*view)->Get(ColumnFamily::kSys, kSysClusterCfg);
  if (!raw.ok()) {
    if (raw.status().IsNotFound()) return std::make_shared<Topology>();
    return raw.status();
  }
  return DecodeClusterCfg(*raw);
}

Status SaveClusterCfg(Storage& storage, const Topology& t) {
  auto txn = storage.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = (*txn)->Put(ColumnFamily::kSys, kSysClusterCfg, EncodeClusterCfg(t)); !st.ok()) return st;
  return (*txn)->Commit();
}

Status DeleteClusterCfg(Storage& storage) {
  auto txn = storage.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = (*txn)->Delete(ColumnFamily::kSys, kSysClusterCfg); !st.ok()) return st;
  return (*txn)->Commit();
}

}  // namespace tideskv::cluster
