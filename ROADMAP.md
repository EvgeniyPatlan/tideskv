# Roadmap

This is the plan from 0.1.0 to 1.0. It is ordered by what users hit first. Items are taken from the [use-case benchmark](bench/compare/README.md) and its command-coverage check against Redis 7.2. Versions are targets, not promises.

## 0.2: storage fixes and quick command wins

**Storage**
- **Reclaim the disk space of expired keys.** After a workload where every key expired, 1.14 GiB was still on disk. The reaper deletes the keys, but the space only comes back when compaction runs over those deletes.
- **Compress values of 1 KB and larger.** Today they are stored as written, while smaller values are compressed.
- **Manual compaction (`COMPACT`).** Lets an operator reclaim space on demand. Kvrocks has this.
- **Faster startup.** First PING after a restart takes 160–400 ms, about twice as long as Kvrocks.
- **Durable-write tail latency.** p99 at 8 connections with `sync-mode full` is 1.69 ms (Kvrocks: 0.50 ms). Look at group-commit batching.

**Observability**
- **Block cache hit and miss counters in `INFO`.** Cache efficiency can't be measured today.
- **`SLOWLOG`, `LATENCY LATEST`, `MEMORY USAGE`.**
- **`CLIENT LIST`, `CLIENT INFO`.**

**Small commands**
- **Keyspace:** `TOUCH`, `COPY`, `MOVE`, `SWAPDB`.
- **Server:** `TIME`, `LASTSAVE`, `OBJECT ENCODING`/`OBJECT FREQ`, `CONFIG GET databases`.
- **Introspection:** the shape of `COMMAND INFO` and `COMMAND DOCS` replies, to match Redis 7.2.

## 0.3: Pub/Sub, bit-level types, notifications

- **Pub/Sub:** `PUBLISH`, `SUBSCRIBE`, `PSUBSCRIBE`, `PUBSUB`, and sharded `SPUBLISH`/`SSUBSCRIBE` in cluster mode. It needs no storage, so it is the cheapest of the missing types.
- **Keyspace notifications:** `notify-keyspace-events`. Builds on Pub/Sub.
- **Bitmaps:** `SETBIT`, `GETBIT`, `BITCOUNT`, `BITPOS`, `BITOP`, `BITFIELD`.
- **HyperLogLog:** `PFADD`, `PFCOUNT`, `PFMERGE`, in Redis' encoding so `DUMP` payloads stay compatible.
- **Client-side caching:** `CLIENT TRACKING`.

## 0.4: Streams and the rest of the core

- **Streams:** `XADD`, `XRANGE`/`XREVRANGE`, `XREAD` (including blocking), `XLEN`, `XDEL`, `XTRIM`, consumer groups (`XGROUP`, `XREADGROUP`, `XACK`, `XPENDING`, `XCLAIM`, `XAUTOCLAIM`) and `XINFO`. The largest item on this list: it needs its own column-family layout and the blocking machinery that lists already use.
- **Geo:** `GEOADD`, `GEODIST`, `GEOPOS`, `GEOSEARCH`, `GEOHASH`. Built on sorted sets.
- **`SORT` / `SORT_RO`.**
- **`DUMP` / `RESTORE`:** for migration tools.
- **Functions:** `FUNCTION LOAD/LIST/DELETE`, `FCALL`, `FCALL_RO`. Built on the Lua engine.

## 1.0: release gate

- **Security:** TLS, and ACLs (`ACL SETUSER` and related commands, with per-user command and key permissions).
- **Packaging:** container image, `.deb`/`.rpm`, and a systemd unit.
- **Benchmark:**
  - Explain why Kvrocks' footprint after a bulk load varied by 2.6× between identical runs.
  - Report disk with and without Kvrocks' WAL archive.
  - Publish the full run: larger-than-RAM datasets, three repetitions.
- **Freeze:** the on-disk format and configuration names, with an upgrade path from 0.x.

## After 1.0

- **JSON** (`JSON.*`), **Search** (`FT.*`) and **Bloom filters** (`BF.*`). These are modules in Redis and built-ins in Kvrocks.

## Non-goals

- On-disk compatibility with Redis RDB or Kvrocks data directories.
