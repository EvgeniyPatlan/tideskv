# TidesKV vs Kvrocks: use-case suite

> **quick: not a disk result, no verdicts**

Run 20261116T144637Z on 12th Gen Intel(R) Core(TM) i9-12900HK, kernel 6.8.0-147-generic. TidesKV 5db560b against apache/kvrocks@sha256:a1e99b3653fb900d5a17b80a9487f74079edc6d5e9b70e3103c01cbfc0d5146a.

## Summary

| Case | Models | Verdict | Ratio median [range] | vs Kvrocks config | Reason |
|---|---|---|---|---|---|
| U1 | a read-heavy cache whose hot keys are a small part of a dataset larger than memory | no verdict (quick) | 1.48 [1.48–1.48] | Kvrocks | closed-gauss ops/s: TidesKV 291,096 vs Kvrocks 197,297 |
| U2 | a session or token store where every key expires | no verdict (quick) | 0.38 [0.38–0.38] | Kvrocks; other metrics: Kvrocks+cron | disk at end (allocated, excl. WAL archive): TidesKV 1.14 GiB vs Kvrocks 448.7 MiB |
| U3 | a long-lived counter or profile store updated in place | no verdict (quick) | 1.41 [1.41–1.41] | Kvrocks | closed ops/s: TidesKV 274,932 vs Kvrocks 195,217 |
| U4 | records stored as hashes with many small fields | no verdict (quick) | 1.34 [1.34–1.34] | Kvrocks | mix ops/s: TidesKV 207,734 vs Kvrocks 155,372 |
| U5 | values of 1, 4 and 16 KB (user records, small documents) | no verdict (quick) | 1.50 [1.50–1.50] | Kvrocks; other metrics: Kvrocks+BlobDB | v4k ops/s: TidesKV 194,039 vs Kvrocks 129,592 |
| U6 | a store that must not lose acknowledged writes | no verdict (quick) | 1.94 [1.94–1.94] | Kvrocks | c64 ops/s: TidesKV 4,338 vs Kvrocks 2,239 |
| U7 | a server that is restarted or killed under write load | no verdict (quick) | 1.00 | Kvrocks | lost writes, all points: TidesKV 0 vs Kvrocks 0 |
| U8 | an initial import and the disk space it takes | no verdict (quick) | 0.97 [0.97–0.97] | Kvrocks | disk after load (allocated, excl. WAL archive): TidesKV 1.50 GiB vs Kvrocks 1.46 GiB |

## Pros and cons

No verdicts in a quick run, so the performance cases draw no pros and cons; the F1 command results below do.

**TidesKV pros**

- F1 strings: TidesKV matches Redis 7.2 on all 18 cases; Kvrocks has 1 undocumented case(s)
- F1 expire: TidesKV matches Redis 7.2 on all 12 cases; Kvrocks has 3 undocumented case(s)
- F1 hash: TidesKV matches Redis 7.2 on all 11 cases; Kvrocks has 1 undocumented case(s)
- F1 list: TidesKV matches Redis 7.2 on all 13 cases; Kvrocks has 4 undocumented case(s)
- F1 set: TidesKV matches Redis 7.2 on all 12 cases; Kvrocks has 1 undocumented case(s)
- F1 zset: TidesKV matches Redis 7.2 on all 19 cases; Kvrocks has 2 undocumented case(s)
- F1 transactions: TidesKV matches Redis 7.2 on all 14 cases; Kvrocks has 1 undocumented case(s)

**TidesKV cons**

- F1: TidesKV refuses commands Redis 7.2 accepts, undocumented: keyspace, scripting, server/info, tracking (14 commands: TOUCH, DUMP, SORT, FUNCTION LIST, TIME, CLIENT INFO, SLOWLOG LEN, SLOWLOG RESET, SLOWLOG GET, MEMORY USAGE, LATENCY LATEST, LASTSAVE and 2 more)
- F1: TidesKV different array content, undocumented: server/info, notifications (3 commands: CONFIG GET, COMMAND INFO, COMMAND DOCS)
- F1: TidesKV status replaced by error ERR, undocumented: notifications (1 command: CONFIG SET)
- F1 type: TidesKV has no Streams, Kvrocks has
- F1 type: TidesKV has no Bitmaps, Kvrocks has
- F1 type: TidesKV has no HyperLogLog, Kvrocks has
- F1 type: TidesKV has no Geo, Kvrocks has
- F1 type: TidesKV has no Pub/Sub, Kvrocks has
- F1 type: TidesKV has no JSON, Kvrocks has
- F1 type: TidesKV has no Search, Kvrocks has
- F1 type: TidesKV has no Bloom, Kvrocks has

**Kvrocks pros**

- F1 type: Kvrocks has Streams, TidesKV does not
- F1 type: Kvrocks has Bitmaps, TidesKV does not
- F1 type: Kvrocks has HyperLogLog, TidesKV does not
- F1 type: Kvrocks has Geo, TidesKV does not
- F1 type: Kvrocks has Pub/Sub, TidesKV does not
- F1 type: Kvrocks has JSON, TidesKV does not
- F1 type: Kvrocks has Search, TidesKV does not
- F1 type: Kvrocks has Bloom, TidesKV does not

**Kvrocks cons**

- F1: Kvrocks error WRONGTYPE replaced by error ERR, undocumented: strings, hash, list, set, zset (17 commands: GET, APPEND, INCR, STRLEN, HGET, HSET, HLEN, LPUSH, LRANGE, LLEN, SADD, SMEMBERS and 5 more)
- F1: Kvrocks error ERR replaced by array, undocumented: keyspace (1 command: SCAN)
- F1: Kvrocks nil-bulk replaced by bulk, undocumented: keyspace, expire (2 commands: RANDOMKEY, GET)
- F1: Kvrocks refuses commands Redis 7.2 accepts, undocumented: keyspace, server/info (4 commands: TOUCH, SWAPDB, COMMAND DOCS, LATENCY LATEST)
- F1: Kvrocks different integer content, undocumented: keyspace, expire (3 commands: EXISTS, PTTL, TTL)
- F1: Kvrocks integer replaced by error ERR, undocumented: expire, server/info (2 commands: EXPIRE, WAIT)
- F1: Kvrocks different array content, undocumented: list, zset, transactions, server/info (7 commands: LPOS, ZMPOP, EXEC, CONFIG GET, HELLO, ROLE, COMMAND INFO)
- F1: Kvrocks nil-array replaced by nil-bulk, undocumented: list (3 commands: BLPOP, LMPOP, BLMPOP)
- F1: Kvrocks status replaced by bulk, undocumented: scripting (1 command: EVAL)
- F1: Kvrocks error ERR replaced by error Invalid, undocumented: scripting (1 command: EVAL)
- F1: Kvrocks different bulk content, undocumented: scripting, geo (2 commands: EVAL, GEODIST)
- F1: Kvrocks error WRONGPASS replaced by error ERR, undocumented: server/info (1 command: AUTH)
- F1: Kvrocks status replaced by error ERR, undocumented: server/info (2 commands: FLUSHDB, FLUSHALL)

**Even (inside the noise band)**

- none

## Per-case results

### U1: Cache bigger than RAM, skewed reads

**Question.** How do the two servers serve skewed GET-heavy traffic from a dataset bigger than the block cache, closed-loop and at equal offered load?

**Setup.** Durability level: relaxed. Repetitions: 1.
Headline cell: ratio 1:10, 1024 B values, key pattern G:G, keys up to 2000000, 4 threads x 50 clients, pipeline 1, 30 s.
Dataset:mem after the load, from on-disk bytes excluding the WAL archive (not the logical size), against SERVER_MEM 8g: TidesKV 0.19 (1.50 GiB); Kvrocks 0.18 (1.46 GiB).
**Note: data fits in page cache on at least one side, so this case does not exercise the disk.**

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode none`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Ratio | Verdict |
|---|---|---|---|---|
| closed-gauss ops/s | 291,096 | 197,297 | 1.48 (band ±5%) | no verdict (quick run) |
| closed-uniform ops/s | 274,388 | 181,347 | 1.51 (band ±5%) | no verdict (quick run) |
| block cache hit rate (closed-gauss) † | n/a | 98.8% | – | info |

- † block cache hit rate (closed-gauss): Kvrocks only: TidesKV's INFO has no hit/miss counters.

**Caveats.**

- Cache sizing: TidesDB charges its index and bloom blocks to its block cache, as Kvrocks does with `cache_index_and_filter_blocks yes`, so both sides have 2048 MB of total read cache.
- Closed-loop latency is not compared across servers (they run at different throughputs): latency verdicts come from the open-loop phases, which run in full mode only.
- Disk numbers exclude Kvrocks' WAL archive.

### U2: TTL churn and space reclaim

**Question.** Does disk usage follow the live data when everything written expires, and what does the churn cost in tail latency? Kvrocks runs twice: default and with a per-minute compaction checker.

**Setup.** Durability level: relaxed. Repetitions: 1.

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode none`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`
- Kvrocks+cron: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`, `compaction-checker-cron * * * * *`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Kvrocks+cron | Ratio | Verdict |
|---|---|---|---|---|---|
| disk at end (allocated, excl. WAL archive) | 1.14 GiB | 448.7 MiB | 678.1 MiB | 0.38 (band ±5%) | no verdict vs Kvrocks (quick run) |
| disk at end (apparent size, excl. WAL archive) | 1.14 GiB | 334.4 MiB | 397.6 MiB | – | info |
| churn p99.9 | 3.535 ms | 4.447 ms | 5.215 ms | 1.26 (band ±5%) | no verdict vs Kvrocks (quick run) |
| peak disk (allocated, excl. WAL archive) | 1.89 GiB | 1.20 GiB | 931.3 MiB | 0.48 (band ±5%) | no verdict vs Kvrocks+cron (quick run) |
| churn ops/s (fixed rate) | 5,000 | 4,998 | 4,999 | – | info |
| GET hit rate (negative lookups over a 1e9-key space) † | 0.0% | 0.0% | 0.0% | – | info |
| WAL archive at end (allocated) | 0 B | 1.34 GiB | 1.50 GiB | – | info |

- † GET hit rate (negative lookups over a 1e9-key space): most GETs ask for keys never written, so a low rate is by design; reported, not checked.

**End state** (live bytes is an estimate: keys left x value size, not a measurement):

| Config | rep | keys at end | value size | live bytes (estimate) | disk at end (allocated) | disk / live |
|---|---|---|---|---|---|---|
| TidesKV | 1 | 0 | 1024 | – | 1.14 GiB | – |
| Kvrocks | 1 | 0 | 1024 | – | 448.7 MiB | – |
| Kvrocks+cron | 1 | 0 | 1024 | – | 678.1 MiB | – |

**Timeline: disk bytes (excluding WAL archive) at each du sample**, one scale from 0 to 1.89 GiB for all lines (data: `U2/timeline.csv`):

- TidesKV rep1 `▁▂▂▃▃▃▃▄▄▅▅▅▆▆▆▇▇██▅▅▅▅▅▅▅▅▅▅▅▅` first 64.0 MiB, last 1.14 GiB, max 1.89 GiB
- Kvrocks rep1 `▂▂▂▃▃▃▃▃▃▄▄▄▄▄▄▅▅▅▄▃▃▃▃▃▃▃▃▃▃▃▃` first 4.3 MiB, last 448.7 MiB, max 1.20 GiB
- Kvrocks+cron rep1 `▂▂▂▃▃▃▃▃▃▄▄▄▄▄▄▄▄▄▃▃▃▃▃▃▃▃▃▃▃▃▃` first 4.3 MiB, last 678.1 MiB, max 931.3 MiB

**Caveats.**

- Neither server had a manual COMPACT: forcing one on a single side would not be a like-for-like comparison.
- GETs draw keys from a 1e9-key space, so nearly all are negative lookups; the hit rate is reported as information, not checked.
- Disk numbers exclude Kvrocks' WAL archive.

### U3: Aged store, small updates, long run

**Question.** After the keyspace has been overwritten twice, how stable is tail latency over a long mixed run, and how much background work is the server carrying?

**Setup.** Durability level: relaxed. Repetitions: 1.
Headline cell: ratio 4:1, 200 B values, key pattern R:R, keys up to 1000000, 4 threads x 50 clients, pipeline 1, 300 s.
Dataset:mem after the load, from on-disk bytes excluding the WAL archive (not the logical size), against SERVER_MEM 8g: TidesKV 0.02 (158.4 MiB); Kvrocks 0.06 (487.9 MiB).
**Note: data fits in page cache on at least one side, so this case does not exercise the disk.**

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode none`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Ratio | Verdict |
|---|---|---|---|---|
| closed p99.9 (not compared: servers run at different loads) | 6.623 ms | 9.087 ms | – | info |
| closed ops/s | 274,932 | 195,217 | 1.41 (band ±5%) | no verdict (quick run) |
| windows over 10x median p99.9 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| closed disk writes per op (server cgroup write bytes / requests) † | 44 B/op | 417 B/op | – | info |

- † closed disk writes per op (server cgroup write bytes / requests): window-only: writes deferred past the timed run (memtable-resident data, later flushes and compactions) are not counted, so it is not compared.

**Background debt** at the start and end of the closed run (information only; the two engines report different things):

- TidesKV rep1: storage_memtable_queue_bytes 0 -> 0, storage_writes_throttled 0 -> 0, storage_writes_blocked 0 -> 0, storage_write_stall_us 0 -> 0, background_compactions 0 -> 0, background_compaction_failures 0 -> 0
- Kvrocks rep1: estimate_pending_compaction_bytes 0 -> 0, compaction_pending 0 -> 0, num_running_compactions 0 -> 0, memtable_flush_pending 0 -> 0, num_running_flushes 0 -> 0, is_compacting no -> no

**Timeline: p99.9 per 10 s window (max of ten one-second points)**, one scale from 0 to 20.095 ms for all lines (data: `U3/timeline.csv`):

- TidesKV rep1 `▃▄▃▄▃▄▄▄▄▄▃▄▃▃▄▄▄▄▃▄▆▄▄▃█▄▃▄▄▄` first 6.687 ms, last 7.487 ms, max 20.095 ms
- Kvrocks rep1 `▅▆▅▅▆▆▆▅▅▅▆▅▆▄▅▆▆▆▆▅▅▅▆▅▆▅▆▅▄▄` first 11.775 ms, last 9.663 ms, max 15.615 ms

**Caveats.**

- Both servers had the same aging. Background debt is shown as information, not as a verdict.

### U4: Hashes as records

**Question.** How fast are HGET, HSET, HGETALL and HDEL on a dataset of 20-field hashes?

**Setup.** Durability level: relaxed. Repetitions: 1.
Headline cell: commands HGET 10, HSET 6, HGETALL 3, HDEL 1 (weights), 50 B values, key pattern R:R, keys up to 250000, 4 threads x 50 clients, pipeline 1, 30 s.
Dataset:mem after the load, from on-disk bytes excluding the WAL archive (not the logical size), against SERVER_MEM 8g: TidesKV 0.03 (271.5 MiB); Kvrocks 0.08 (667.7 MiB).
**Note: data fits in page cache on at least one side, so this case does not exercise the disk.**

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode none`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Ratio | Verdict |
|---|---|---|---|---|
| mix ops/s | 207,734 | 155,372 | 1.34 (band ±5%) | no verdict (quick run) |
| mix p99 (not compared: closed loop) | 1.783 ms | 2.351 ms | – | info |
| HGET ops/s † | 103,884 | 77,703 | 1.34 (band ±5%) | no verdict (quick run) |
| HSET ops/s † | 62,314 | 46,605 | 1.34 (band ±5%) | no verdict (quick run) |
| HGETALL ops/s † | 31,153 | 23,298 | 1.34 (band ±5%) | no verdict (quick run) |
| HDEL ops/s † | 10,384 | 7,765 | 1.34 (band ±5%) | no verdict (quick run) |

- † HGET ops/s: fixed command ratio: follows mix ops/s.
- † HSET ops/s: fixed command ratio: follows mix ops/s.
- † HGETALL ops/s: fixed command ratio: follows mix ops/s.
- † HDEL ops/s: fixed command ratio: follows mix ops/s.

**Caveats.**

- HGET/HGETALL replies are not hit-counted by memtier, so the reply check covers the integer replies of HSET and HDEL only.
- memtier picks each command by its weight on its own, so HDEL runs without a paired HSET (the spec's "HDEL+HSET 5%" is HDEL alone at weight 1); a deleted field is set again by a later HSET. The same command stream runs on both servers.
- Per-command ops/s follow the mix ops/s (the command ratio is fixed) and are not counted separately.

### U5: Mid-size values

**Question.** How do throughput, tail latency and disk footprint change with value size? Kvrocks runs with and without BlobDB.

**Setup.** Durability level: relaxed. Repetitions: 1.
Headline cell: ratio 1:4, 4096 B values, key pattern R:R, keys up to 131072, 4 threads x 50 clients, pipeline 1, 30 s.
Dataset:mem after the load (the 16 KB phase), from on-disk bytes excluding the WAL archive (not the logical size), against SERVER_MEM 8g: TidesKV 0.05 (448.0 MiB); Kvrocks 0.10 (799.5 MiB); Kvrocks+BlobDB 0.06 (491.2 MiB).
**Note: data fits in page cache on at least one side, so this case does not exercise the disk.**

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode none`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`
- Kvrocks+BlobDB: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`, `rocksdb.enable_blob_files yes`, `rocksdb.min_blob_size 4096`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Kvrocks+BlobDB | Ratio | Verdict |
|---|---|---|---|---|---|
| v1k ops/s | 249,531 | 214,279 | 213,972 | 1.16 (band ±5%) | no verdict vs Kvrocks (quick run) |
| v1k p99 (not compared: closed loop) | 1.687 ms | 3.855 ms | 3.967 ms | – | info |
| v1k disk writes per op (server cgroup write bytes / requests) † | 223 B/op | 655 B/op | 647 B/op | – | info |
| v1k disk after load (allocated, excl. WAL archive) | 448.0 MiB | 732.7 MiB | 732.3 MiB | 1.63 (band ±5%) | no verdict vs Kvrocks+BlobDB (quick run) |
| v1k disk after load (apparent size, excl. WAL archive) | 448.0 MiB | 529.0 MiB | 528.7 MiB | – | info |
| v4k ops/s | 194,039 | 129,592 | 125,960 | 1.50 (band ±5%) | no verdict vs Kvrocks (quick run) |
| v4k p99 (not compared: closed loop) | 1.679 ms | 6.047 ms | 6.335 ms | – | info |
| v4k disk writes per op (server cgroup write bytes / requests) † | 751 B/op | 2,474 B/op | 1,523 B/op | – | info |
| v4k disk after load (allocated, excl. WAL archive) | 448.0 MiB | 735.7 MiB | 520.4 MiB | 1.16 (band ±5%) | no verdict vs Kvrocks+BlobDB (quick run) |
| v4k disk after load (apparent size, excl. WAL archive) | 448.0 MiB | 517.8 MiB | 302.5 MiB | – | info |
| v16k ops/s | 107,497 | 48,449 | 71,375 | 1.51 (band ±5%) | no verdict vs Kvrocks+BlobDB (quick run) |
| v16k p99 (not compared: closed loop) | 3.887 ms | 17.151 ms | 12.031 ms | – | info |
| v16k disk writes per op (server cgroup write bytes / requests) † | 2,326 B/op | 9,910 B/op | 5,364 B/op | – | info |
| v16k disk after load (allocated, excl. WAL archive) | 448.0 MiB | 799.5 MiB | 491.2 MiB | 1.10 (band ±5%) | no verdict vs Kvrocks+BlobDB (quick run) |
| v16k disk after load (apparent size, excl. WAL archive) | 448.0 MiB | 515.2 MiB | 315.3 MiB | – | info |

- † v1k disk writes per op (server cgroup write bytes / requests): window-only: writes deferred past the timed run (memtable-resident data, later flushes and compactions) are not counted, so it is not compared.
- † v4k disk writes per op (server cgroup write bytes / requests): window-only: writes deferred past the timed run (memtable-resident data, later flushes and compactions) are not counted, so it is not compared.
- † v16k disk writes per op (server cgroup write bytes / requests): window-only: writes deferred past the timed run (memtable-resident data, later flushes and compactions) are not counted, so it is not compared.

**Caveats.**

- Both Kvrocks configurations are shown so that neither blob threshold is cherry-picked.
- Disk numbers exclude Kvrocks' WAL archive.

### U6: Durable writes, connection sweep

**Question.** What does an fsync per acknowledged write cost at 1, 8, 64 and 256 connections, against the disk's own durable-append floor?

**Setup.** Durability level: strict. Repetitions: 1.
Headline cell: ratio 1:0, 100 B values, key pattern R:R, keys up to 1000000, 4 threads x 16 clients, pipeline 1, 15 s.
Dataset:mem after the load, from on-disk bytes excluding the WAL archive (not the logical size), against SERVER_MEM 8g: TidesKV 0.02 (192.0 MiB); Kvrocks 0.03 (285.9 MiB).

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode full`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync yes`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Ratio | Verdict |
|---|---|---|---|---|
| c1 ops/s | 2,752 | 1,026 | 2.68 (band ±5%) | no verdict (quick run) |
| c1 p99 (not compared: closed loop) | 1.103 ms | 4.543 ms | – | info |
| c1 ops/s over the durable-append floor | 11.63x | 4.83x | – | info |
| c8 ops/s | 3,196 | 1,634 | 1.96 (band ±5%) | no verdict (quick run) |
| c8 p99 (not compared: closed loop) | 9.343 ms | 22.783 ms | – | info |
| c8 ops/s over the durable-append floor | 13.50x | 7.69x | – | info |
| c64 ops/s | 4,338 | 2,239 | 1.94 (band ±5%) | no verdict (quick run) |
| c64 p99 (not compared: closed loop) | 50.175 ms | 113.151 ms | – | info |
| c64 ops/s over the durable-append floor | 18.32x | 10.54x | – | info |
| c256 ops/s | 5,483 | 1,076 | 5.10 (band ±5%) | no verdict (quick run) |
| c256 p99 (not compared: closed loop) | 92.159 ms | 659.455 ms | – | info |
| c256 ops/s over the durable-append floor | 23.16x | 5.06x | – | info |

- TidesKV rep1 c64: unstable: ops/s per second varied with a coefficient of variation of 0.57 (limit 0.25), no verdict for this cell.
- TidesKV rep1 c8: unstable: ops/s per second varied with a coefficient of variation of 0.65 (limit 0.25), no verdict for this cell.
- Kvrocks rep1 c1: unstable: ops/s per second varied with a coefficient of variation of 0.52 (limit 0.25), no verdict for this cell.
- Kvrocks rep1 c256: unstable: ops/s per second varied with a coefficient of variation of 0.60 (limit 0.25), no verdict for this cell.
- Kvrocks rep1 c64: unstable: ops/s per second varied with a coefficient of variation of 0.50 (limit 0.25), no verdict for this cell.
- Kvrocks rep1 c8: unstable: ops/s per second varied with a coefficient of variation of 0.75 (limit 0.25), no verdict for this cell.

**Disk floors** (fio, queue depth 1, right before each server's sweep). The durable-append floor is the write pattern of a write-ahead log and the reference for the "ops/s over the durable-append floor" rows; the random-write job is kept as a reference:

- durable-append floor (4 KiB sequential write + fdatasync), start of run: 1621 writes/s, fsync latency mean 0.603 ms, p99 1.843 ms
- durable-append floor (4 KiB sequential write + fdatasync), TidesKV rep1: 237 writes/s, fsync latency mean 4.207 ms, p99 6.390 ms
- durable-append floor (4 KiB sequential write + fdatasync), Kvrocks rep1: 212 writes/s, fsync latency mean 4.688 ms, p99 7.832 ms
- fio reference (4 KiB random write + fsync), start of run: 1498 writes/s, fsync latency mean 0.652 ms, p99 2.212 ms
- fio reference (4 KiB random write + fsync), TidesKV rep1: 196 writes/s, fsync latency mean 5.089 ms, p99 8.585 ms
- fio reference (4 KiB random write + fsync), Kvrocks rep1: 234 writes/s, fsync latency mean 4.245 ms, p99 5.341 ms

**Caveats.**

- Strict durability on both sides.
- The disk's flush rate drifts, so both fio jobs were measured right before each server's sweep (above).
- Strict mode is configured and read back on both servers, but per-ack fsync is not observed by the suite: a server faster than the durable-append floor batches flushes (group commit) or does not flush per write; the suite cannot tell which.
- Writes per fsync: not observable. The INFO of neither server reports an fsync count (its sync_* fields are replication counters), so group-commit batch size cannot be read.
- A cell whose per-second ops vary by more than a quarter (coefficient of variation above 0.25) is flagged unstable and gets no verdict: the disk's fsync rate was not steady.

### U7: Restart and crash recovery

**Question.** After SIGTERM or SIGKILL under write load at strict durability, is any acknowledged write lost, and how long until the server answers and is fast again?

**Setup.** Durability level: strict. Repetitions: 1.
Dataset:mem after the load, from on-disk bytes excluding the WAL archive (not the logical size), against SERVER_MEM 8g: TidesKV 0.01 (64.0 MiB); Kvrocks 0.03 (285.9 MiB).

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode full`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync yes`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Ratio | Verdict |
|---|---|---|---|---|
| lost writes, all points | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| corrupt values, all points | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| restart misses, all points | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| first PONG, all points | 134.500 ms | 90.500 ms | 0.67 (band ±5%) | no verdict (quick run) |
| recovered (p99 within 2x), all points | 338.500 ms | 505.000 ms | 1.49 (band ±5%) | no verdict (quick run) |
| lost writes, term-1271 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| corrupt values, term-1271 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| restart misses, term-1271 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| first PONG, term-1271 | 436.000 ms | 267.000 ms | 0.61 (band ±5%) | no verdict (quick run) |
| recovered (p99 within 2x), term-1271 | 436.000 ms | 367.000 ms | 0.84 (band ±5%) | no verdict (quick run) |
| docker run (not in PONG time), term-1271 | 217.000 ms | 242.000 ms | – | info |
| acked writes at the signal (target 1271), term-1271 | 1,281 | 1,293 | – | info |
| recovery threshold (2x warm p99), term-1271 | 0.068 ms | 0.064 ms | – | info |
| TERM shutdown, term-1271 | 0.2 s | 0.3 s | – | info |
| lost writes, term-6355 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| corrupt values, term-6355 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| restart misses, term-6355 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| first PONG, term-6355 | 87.000 ms | 81.000 ms | 0.93 (band ±5%) | no verdict (quick run) |
| recovered (p99 within 2x), term-6355 | 188.000 ms | 482.000 ms | 2.56 (band ±5%) | no verdict (quick run) |
| docker run (not in PONG time), term-6355 | 134.000 ms | 206.000 ms | – | info |
| acked writes at the signal (target 6355), term-6355 | 6,577 | 6,369 | – | info |
| recovery threshold (2x warm p99), term-6355 | 0.064 ms | 0.084 ms | – | info |
| TERM shutdown, term-6355 | 0.2 s | 0.2 s | – | info |
| lost writes, term-11439 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| corrupt values, term-11439 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| restart misses, term-11439 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| first PONG, term-11439 | 141.000 ms | 98.000 ms | 0.70 (band ±5%) | no verdict (quick run) |
| recovered (p99 within 2x), term-11439 | 241.000 ms | 499.000 ms | 2.07 (band ±5%) | no verdict (quick run) |
| docker run (not in PONG time), term-11439 | 140.000 ms | 150.000 ms | – | info |
| acked writes at the signal (target 11439), term-11439 | 11,657 | 11,539 | – | info |
| recovery threshold (2x warm p99), term-11439 | 0.068 ms | 0.142 ms | – | info |
| TERM shutdown, term-11439 | 0.2 s | 0.2 s | – | info |
| lost writes, kill-1271 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| corrupt values, kill-1271 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| restart misses, kill-1271 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| first PONG, kill-1271 | 74.000 ms | 49.000 ms | 0.66 (band ±5%) | no verdict (quick run) |
| recovered (p99 within 2x), kill-1271 | 476.000 ms | 651.000 ms | 1.37 (band ±5%) | no verdict (quick run) |
| docker run (not in PONG time), kill-1271 | 213.000 ms | 251.000 ms | – | info |
| acked writes at the signal (target 1271), kill-1271 | 1,290 | 1,303 | – | info |
| recovery threshold (2x warm p99), kill-1271 | 0.070 ms | 0.078 ms | – | info |
| lost writes, kill-6355 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| corrupt values, kill-6355 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| restart misses, kill-6355 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| first PONG, kill-6355 | 128.000 ms | 83.000 ms | 0.65 (band ±5%) | no verdict (quick run) |
| recovered (p99 within 2x), kill-6355 | 228.000 ms | 1086.000 ms | 4.76 (band ±5%) | no verdict (quick run) |
| docker run (not in PONG time), kill-6355 | 159.000 ms | 160.000 ms | – | info |
| acked writes at the signal (target 6355), kill-6355 | 6,531 | 6,481 | – | info |
| recovery threshold (2x warm p99), kill-6355 | 0.054 ms | 0.070 ms | – | info |
| lost writes, kill-11439 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| corrupt values, kill-11439 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| restart misses, kill-11439 | 0 | 0 | 1.00 (band ±5%) | no verdict (quick run) |
| first PONG, kill-11439 | 189.000 ms | 110.000 ms | 0.58 (band ±5%) | no verdict (quick run) |
| recovered (p99 within 2x), kill-11439 | 491.000 ms | 511.000 ms | 1.04 (band ±5%) | no verdict (quick run) |
| docker run (not in PONG time), kill-11439 | 148.000 ms | 147.000 ms | – | info |
| acked writes at the signal (target 11439), kill-11439 | 11,582 | 11,497 | – | info |
| recovery threshold (2x warm p99), kill-11439 | 0.064 ms | 0.156 ms | – | info |

Per-point rows (SIGTERM and SIGKILL at each acked-write count) are information; the "all points" rows are the ones counted in the case label and the pros and cons. A server that never recovered shows ERR with the cause.

**Caveats.**

- This is a process kill (SIGTERM/SIGKILL), not an OS crash or power loss.
- pong_ms and recovered_ms are measured from docker run returning; docker's own time is shown separately.
- The recovery threshold is 2x the warm pre-kill p99 of the same server; each server's absolute threshold is in the table, since a lower warm p99 makes the 2x rule stricter for that server.
- Each point (the number in its name) signals after the same number of acknowledged writes on both servers, about 10%, 50% and 90% of the 256 MiB memtable at 2 KiB values (a tenth of that in quick mode); the acked-at-signal row is the count when the signal was sent (the 50 ms poll overshoots a little). The fill counts the writer's writes only, not the preload or what a restart's WAL replay leaves in the memtable. A point that did not reach its count in time is ERR.

### U8: Bulk load and footprint

**Question.** How fast does each server take a bulk load, how long until it is quiet, and how much disk does it occupy after load, after aging, and (Kvrocks) after a full compaction?

**Setup.** Durability level: relaxed. Repetitions: 1.

- TidesKV: `workers 4`, `block-cache-size 2147483648`, `memtable-size 268435456`, `compression lz4`, `sync-mode none`
- Kvrocks: `workers 4`, `rocksdb.block_cache_size 2048`, `rocksdb.write_buffer_size 256`, `rocksdb.compression lz4`, `rocksdb.write_options.sync no`

**Results** (median [min–max] over repetitions; ratio above 1 means TidesKV is better):

| Metric | TidesKV | Kvrocks | Ratio | Verdict |
|---|---|---|---|---|
| disk after load (allocated, excl. WAL archive) | 1.50 GiB | 1.46 GiB | 0.97 (band ±5%) | no verdict (quick run) |
| disk after load (apparent size, excl. WAL archive) | 1.50 GiB | 1.24 GiB | – | info |
| bulk load ops/s † | 265,604 | 107,267 | 2.48 (band ±5%) | no verdict (quick run) |
| load ops/s, controlled payload (U1's load) † | 301,386 | 164,352 | – | info |
| Kvrocks SST bytes stored uncompressed after load † | n/a | 37.2% | – | info |
| disk after the memtier load (allocated, excl. WAL archive) † | 2.44 GiB | 598.4 MiB | – | info |
| time to quiet after U1's load (controlled payload) | ≤ 5 s (floor) | ≤ 5 s (floor) | 1.00 (band ±5%) | no verdict (quick run) |
| U3 disk after its load (allocated, excl. WAL archive) | 158.4 MiB | 487.9 MiB | – | info |
| U3 disk growth during aging (allocated, excl. WAL archive) | 202.3 MiB | 145.9 MiB | – | info |
| disk after U3 aging (allocated, excl. WAL archive) | 360.7 MiB | 633.7 MiB | 1.76 (band ±5%) | no verdict (quick run) |
| disk after COMPACT (allocated, excl. WAL archive) † | n/a | 1019.9 MiB | – | info |

- † bulk load ops/s: memtier load of U1's key set (1 KiB values, pipeline 32, 4 x 8 connections) on an empty server; the footprints come from the controlled-payload loads instead.
- † load ops/s, controlled payload (U1's load): uc_loader.py at pipeline 32 over 32 connections, values that compress to about half; partly a measure of the loader itself, so not compared.
- † Kvrocks SST bytes stored uncompressed after load: Kvrocks only: its default compression_start_level 2 leaves the levels above the bottom one uncompressed, so this share, set by compaction timing, moves the disk-after-load figure.
- † disk after the memtier load (allocated, excl. WAL archive): not compared: memtier's values compress unrealistically well in blocks, which helps a block compressor only.
- † disk after COMPACT (allocated, excl. WAL archive): Kvrocks only: TidesKV has no COMPACT. A fresh load of U1's data (same values) on an empty server, then a full COMPACT.

**Caveats.**

- TidesKV has no COMPACT, so the compaction figure exists for Kvrocks only. It comes from a fresh load of U1's data on an empty server followed by a full COMPACT, so it is the steady state of the same data.
- Kvrocks' disk after a load depends on how much data compaction has moved into its bottom level, the only compressed one by default; the share left uncompressed is shown beside it.
- The load speed is a memtier load (memtier's own values); the footprints are from loads of values that compress to about half, the same bytes on both servers.
- Time to quiet cannot be shorter than the settle check's window; a value at that floor is shown as "≤ floor" and gets no verdict.
- A settle wait that reached QUIESCE_MAX is censored: shown as "≥ cap", not an error. It gets a verdict only when the other side went quiet below the cap by more than the band; when both reached it there is none ("both capped").
- U8's footprints after load and after aging are U1's and U3's; it runs its own loads only for the speed and the COMPACT figure.
- Disk numbers exclude Kvrocks' WAL archive.

## F1: command coverage and semantics

Reference: redis 7.2.5 at tideskv-bench-redis-20261116T144637Z-17137:6379 (the neutral oracle: a command is `same` when its reply equals Redis'). 171 cases, each on a flushed database, every command of a case compared, one repetition (expiry-timing cases, such as EXPIRE with 0 or a negative time, can differ between runs).

Per case: `differs`: a reply differs from Redis'. `unsupported`: every difference is a refusal, the server answers unknown command or subcommand (or Kvrocks' equivalents, see `is_unknown` in `f1_matrix.py`) where Redis succeeds. `extra`: Redis 7.2 itself answers unknown command and the server answers anything else, so any non-refusal reply counts as having the command (a probe such as `FT.INFO` may reply with an error and still be counted as present). `documented`: how many of the differs and unsupported cases have every difference described in the server's own documentation (the link is in the list below); a case with only some documented differences is `differs` and counts as undocumented. The label applies to a server only through that server's own documentation.

Later commands of a case can differ because an earlier one did (for example after a refused SELECT, OBJECT or SCRIPT KILL); the list gives every differing command, so read the rows of one case together.

Kvrocks ran with its default `redis-databases 0`, where `SELECT` does not switch databases; the setting is configurable ([supported commands](https://kvrocks.apache.org/docs/supported-commands)), so the SELECT-related differences below depend on that choice and a Kvrocks configured with `redis-databases` above 0 would not show them.

Error replies compare by their code word (`ERR`, `WRONGTYPE`, ...), not by their text (see the appendix); `#unordered` and `#length` cases compare sets and lengths of replies, not their order.

On the type probes (streams to bloom) `same` with a `no` in the types table means Redis 7.2 and the server both lack the command.

### Per family

| Family | Cases | TidesKV | Kvrocks |
|---|---|---|---|
| strings | 18 | same 18 / differs 0 / unsupported 0 / documented 0 | same 17 / differs 1 / unsupported 0 / documented 0 |
| keyspace | 18 | same 12 / differs 3 / unsupported 3 / documented 3 | same 9 / differs 7 / unsupported 2 / documented 4 |
| expire | 12 | same 12 / differs 0 / unsupported 0 / documented 0 | same 9 / differs 3 / unsupported 0 / documented 0 |
| hash | 11 | same 11 / differs 0 / unsupported 0 / documented 0 | same 10 / differs 1 / unsupported 0 / documented 0 |
| list | 13 | same 13 / differs 0 / unsupported 0 / documented 0 | same 8 / differs 5 / unsupported 0 / documented 1 |
| set | 12 | same 12 / differs 0 / unsupported 0 / documented 0 | same 11 / differs 1 / unsupported 0 / documented 0 |
| zset | 19 | same 19 / differs 0 / unsupported 0 / documented 0 | same 16 / differs 3 / unsupported 0 / documented 1 |
| transactions | 14 | same 14 / differs 0 / unsupported 0 / documented 0 | same 12 / differs 2 / unsupported 0 / documented 1 |
| scripting | 16 | same 15 / differs 0 / unsupported 1 / documented 0 | same 12 / differs 4 / unsupported 0 / documented 1 |
| server/info | 27 | same 17 / differs 3 / unsupported 6 / documented 1 / extra 1 | same 17 / differs 7 / unsupported 2 / documented 0 / extra 1 |
| notifications | 1 | same 0 / differs 1 / unsupported 0 / documented 0 | same 0 / differs 1 / unsupported 0 / documented 1 |
| tracking | 1 | same 0 / differs 0 / unsupported 1 / documented 0 | same 0 / differs 0 / unsupported 1 / documented 1 |
| object | 1 | same 0 / differs 0 / unsupported 1 / documented 1 | same 0 / differs 0 / unsupported 1 / documented 1 |
| streams | 1 | same 0 / differs 0 / unsupported 1 / documented 1 | same 1 / differs 0 / unsupported 0 / documented 0 |
| bitmaps | 1 | same 0 / differs 0 / unsupported 1 / documented 1 | same 1 / differs 0 / unsupported 0 / documented 0 |
| hyperloglog | 1 | same 0 / differs 0 / unsupported 1 / documented 1 | same 1 / differs 0 / unsupported 0 / documented 0 |
| geo | 1 | same 0 / differs 0 / unsupported 1 / documented 1 | same 0 / differs 1 / unsupported 0 / documented 0 |
| pubsub | 1 | same 0 / differs 0 / unsupported 1 / documented 1 | same 1 / differs 0 / unsupported 0 / documented 0 |
| json | 1 | same 1 / differs 0 / unsupported 0 / documented 0 | same 0 / differs 0 / unsupported 0 / documented 0 / extra 1 |
| search | 1 | same 1 / differs 0 / unsupported 0 / documented 0 | same 0 / differs 0 / unsupported 0 / documented 0 / extra 1 |
| bloom | 1 | same 1 / differs 0 / unsupported 0 / documented 0 | same 0 / differs 0 / unsupported 0 / documented 0 / extra 1 |

### Types only one side has

yes = the first probe command is accepted; no = the server refuses it.

| Type | Probe | Redis 7.2 | TidesKV | Kvrocks |
|---|---|---|---|---|
| Streams | `xadd xlen` | yes | no | yes |
| Bitmaps | `setbit getbit bitcount` | yes | no | yes |
| HyperLogLog | `pfadd pfcount` | yes | no | yes |
| Geo | `geoadd geodist` | yes | no | yes |
| Pub/Sub | `publish` | yes | no | yes |
| JSON | `json set get` | no | no | yes |
| Search | `ft info` | no | no | yes |
| Bloom | `bf add` | no | no | yes |

### Differing commands, per case

| Family | Case | Server | Result | Command | Redis 7.2 | Server | Source |
|---|---|---|---|---|---|---|---|
| strings | wrong type on string commands | Kvrocks | differs | `GET l` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| strings | wrong type on string commands | Kvrocks | differs | `APPEND l x` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| strings | wrong type on string commands | Kvrocks | differs | `INCR l` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| strings | wrong type on string commands | Kvrocks | differs | `STRLEN l` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| keyspace | scan bad cursor | Kvrocks | differs | `SCAN abc` | -ERR invalid cursor | ["0", []] | undocumented |
| keyspace | randomkey dbsize | Kvrocks | differs | `RANDOMKEY` | (nil) | "" | undocumented |
| keyspace | randomkey dbsize | Kvrocks | differs | `DBSIZE` | :1 | :0 | documented: [DBSIZE is updated asynchronously after DBSIZE SCAN](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | unlink touch | TidesKV | unsupported | `TOUCH a b missing` | :2 | -ERR unknown command 'TOUCH', with args beginning with: 'a' 'b' 'missi... | undocumented |
| keyspace | unlink touch | Kvrocks | unsupported | `TOUCH a b missing` | :2 | -ERR unknown command `TOUCH`, with args beginning with: `a`, `b`, `mis... | undocumented |
| keyspace | select database | Kvrocks | differs | `GET a` | (nil) | "1" | documented: [follows from SELECT not switching databases at redis-databas...](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | select database | Kvrocks | differs | `GET a` | "1" | "2" | documented: [follows from SELECT not switching databases at redis-databas...](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | select out of range | Kvrocks | differs | `SELECT 99` | -ERR DB index is out of range | +OK | documented: [SELECT only switches databases when redis-databases is above...](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | select out of range | Kvrocks | differs | `SELECT abc` | -ERR value is not an integer or out of range | +OK | documented: [SELECT only switches databases when redis-databases is above...](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | flushdb isolation | Kvrocks | differs | `DBSIZE` | :1 | :0 | documented: [DBSIZE is updated asynchronously after DBSIZE SCAN, and SELE...](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | dbsize after writes and deletes | Kvrocks | differs | `DBSIZE` | :3 | :0 | documented: [DBSIZE is updated asynchronously after DBSIZE SCAN](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | dbsize after writes and deletes | Kvrocks | differs | `DBSIZE` | :2 | :0 | documented: [DBSIZE is updated asynchronously after DBSIZE SCAN](https://kvrocks.apache.org/docs/supported-commands) |
| keyspace | copy | TidesKV | unsupported | `COPY a b` | :1 | -ERR unknown command 'COPY', with args beginning with: 'a' 'b'  | documented: [COPY is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| keyspace | copy | TidesKV | differs | `GET b` | "1" | (nil) | documented: [follows from the unsupported COPY](docs/tutorial.md#differences-from-redis) |
| keyspace | copy | TidesKV | unsupported | `COPY a b` | :0 | -ERR unknown command 'COPY', with args beginning with: 'a' 'b'  | documented: [COPY is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| keyspace | copy | TidesKV | unsupported | `COPY a b REPLACE` | :1 | -ERR unknown command 'COPY', with args beginning with: 'a' 'b' 'REPLAC... | documented: [COPY is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| keyspace | move | TidesKV | unsupported | `MOVE a 1` | :1 | -ERR unknown command 'MOVE', with args beginning with: 'a' '1'  | documented: [MOVE is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| keyspace | move | TidesKV | differs | `EXISTS a` | :0 | :1 | documented: [follows from the unsupported MOVE](docs/tutorial.md#differences-from-redis) |
| keyspace | move | Kvrocks | differs | `EXISTS a` | :0 | :1 | undocumented |
| keyspace | swapdb | TidesKV | unsupported | `SWAPDB 0 1` | +OK | -ERR unknown command 'SWAPDB', with args beginning with: '0' '1'  | documented: [SWAPDB is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| keyspace | swapdb | TidesKV | differs | `DBSIZE` | :0 | :1 | documented: [follows from the unsupported SWAPDB](docs/tutorial.md#differences-from-redis) |
| keyspace | swapdb | TidesKV | unsupported | `SWAPDB 0 1` | +OK | -ERR unknown command 'SWAPDB', with args beginning with: '0' '1'  | documented: [SWAPDB is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| keyspace | swapdb | Kvrocks | unsupported | `SWAPDB 0 1` | +OK | -ERR unknown command `SWAPDB`, with args beginning with: `0`, `1` | undocumented |
| keyspace | swapdb | Kvrocks | unsupported | `SWAPDB 0 1` | +OK | -ERR unknown command `SWAPDB`, with args beginning with: `0`, `1` | undocumented |
| keyspace | dump restore | TidesKV | unsupported | `DUMP missing` | (nil) | -ERR unknown command 'DUMP', with args beginning with: 'missing'  | undocumented |
| keyspace | sort | TidesKV | unsupported | `SORT l` | ["1", "2", "3"] | -ERR unknown command 'SORT', with args beginning with: 'l'  | undocumented |
| keyspace | sort | TidesKV | unsupported | `SORT l DESC` | ["3", "2", "1"] | -ERR unknown command 'SORT', with args beginning with: 'l' 'DESC'  | undocumented |
| keyspace | sort | TidesKV | unsupported | `SORT l LIMIT 0 2` | ["1", "2"] | -ERR unknown command 'SORT', with args beginning with: 'l' 'LIMIT' '0'... | undocumented |
| keyspace | sort | TidesKV | unsupported | `SORT w ALPHA` | ["a", "b"] | -ERR unknown command 'SORT', with args beginning with: 'w' 'ALPHA'  | undocumented |
| expire | pexpire pttl | Kvrocks | differs | `PTTL a` | :99999 | :100000 | undocumented |
| expire | expire options | Kvrocks | differs | `EXPIRE a 100 NX` | :1 | -ERR wrong number of arguments | undocumented |
| expire | expire options | Kvrocks | differs | `EXPIRE a 200 NX` | :0 | -ERR wrong number of arguments | undocumented |
| expire | expire options | Kvrocks | differs | `EXPIRE a 300 XX` | :1 | -ERR wrong number of arguments | undocumented |
| expire | expire options | Kvrocks | differs | `EXPIRE a 100 GT` | :0 | -ERR wrong number of arguments | undocumented |
| expire | expire options | Kvrocks | differs | `EXPIRE a 100 LT` | :1 | -ERR wrong number of arguments | undocumented |
| expire | expire options | Kvrocks | differs | `TTL a` | :100 | :-1 | undocumented |
| expire | expire negative and zero deletes | Kvrocks | differs | `GET b` | (nil) | "1" | undocumented |
| hash | wrong type | Kvrocks | differs | `HGET s f` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| hash | wrong type | Kvrocks | differs | `HSET s f v` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| hash | wrong type | Kvrocks | differs | `HLEN s` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| list | lpos | Kvrocks | differs | `LPOS l b COUNT 0` | [:1, :3] | ["1", "3"] | undocumented |
| list | brpoplpush | Kvrocks | unsupported | `BRPOPLPUSH src dst 1` | "a" | -ERR unknown command `BRPOPLPUSH`, with args beginning with: `src`, `d... | documented: [BRPOPLPUSH is marked unsupported (deprecated)](https://kvrocks.apache.org/docs/supported-commands) |
| list | brpoplpush | Kvrocks | differs | `LRANGE dst 0 -1` | ["a"] | [] | documented: [follows from the unsupported BRPOPLPUSH (nothing was moved)](https://kvrocks.apache.org/docs/supported-commands) |
| list | brpoplpush | Kvrocks | unsupported | `BRPOPLPUSH src dst 0.1` | (nil array) | -ERR unknown command `BRPOPLPUSH`, with args beginning with: `src`, `d... | documented: [BRPOPLPUSH is marked unsupported (deprecated)](https://kvrocks.apache.org/docs/supported-commands) |
| list | blpop brpop immediate and timeout | Kvrocks | differs | `BLPOP l 0.1` | (nil array) | (nil) | undocumented |
| list | blpop brpop immediate and timeout | Kvrocks | differs | `BLPOP missing other 0.1` | (nil array) | (nil) | undocumented |
| list | lmpop blmpop | Kvrocks | differs | `LMPOP 1 missing LEFT` | (nil array) | (nil) | undocumented |
| list | lmpop blmpop | Kvrocks | differs | `BLMPOP 0.1 1 missing LEFT` | (nil array) | (nil) | undocumented |
| list | wrong type | Kvrocks | differs | `LPUSH s a` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| list | wrong type | Kvrocks | differs | `LRANGE s 0 -1` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| list | wrong type | Kvrocks | differs | `LLEN s` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| set | wrong type | Kvrocks | differs | `SADD s a` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| set | wrong type | Kvrocks | differs | `SMEMBERS s` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| set | wrong type | Kvrocks | differs | `SCARD s` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| set | wrong type | Kvrocks | differs | `SINTER l s` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| zset | bzpopmin and zmpop | Kvrocks | differs | `ZMPOP 1 z MIN COUNT 5` | ["z", [["b", "2"]]] | ["z", ["b", "2"]] | undocumented |
| zset | zrangebylex with mixed scores | Kvrocks | differs | `ZRANGEBYLEX z - +` | ["d", "b", "c", "a"] | ["a", "b", "c", "d"] | documented: [ZRANGEBYLEX/ZREVRANGEBYLEX/ZREMRANGEBYLEX order by member an...](https://kvrocks.apache.org/docs/supported-commands) |
| zset | zrangebylex with mixed scores | Kvrocks | differs | `ZRANGEBYLEX z [b [c` | [] | ["b", "c"] | documented: [ZRANGEBYLEX/ZREVRANGEBYLEX/ZREMRANGEBYLEX order by member an...](https://kvrocks.apache.org/docs/supported-commands) |
| zset | zrangebylex with mixed scores | Kvrocks | differs | `ZREMRANGEBYLEX z [a [b` | :0 | :2 | documented: [ZRANGEBYLEX/ZREVRANGEBYLEX/ZREMRANGEBYLEX order by member an...](https://kvrocks.apache.org/docs/supported-commands) |
| zset | zrangebylex with mixed scores | Kvrocks | differs | `ZRANGE z 0 -1 WITHSCORES` | ["d", "0", "b", "1", "c", "2", "a", "3"] | ["d", "0", "c", "2"] | documented: [follows from ZREMRANGEBYLEX ignoring scores (it removed othe...](https://kvrocks.apache.org/docs/supported-commands) |
| zset | wrong type | Kvrocks | differs | `ZADD s 1 a` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| zset | wrong type | Kvrocks | differs | `ZRANGE s 0 -1` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| zset | wrong type | Kvrocks | differs | `ZCARD s` | -WRONGTYPE Operation against a key holding the wrong kind of value | -ERR Invalid argument: WRONGTYPE Operation against a key holding the w... | undocumented |
| transactions | blocking command inside multi | Kvrocks | differs | `EXEC` | [(nil array)] | [(nil)] | undocumented |
| transactions | multi select | Kvrocks | differs | `GET a` | (nil) | "1" | documented: [follows from SELECT not switching databases at redis-databas...](https://kvrocks.apache.org/docs/supported-commands) |
| scripting | eval return types | Kvrocks | differs | `EVAL return {ok='fine'} 0` | +fine | "fine" | undocumented |
| scripting | redis call error and pcall | Kvrocks | differs | `EVAL return redis.pcall('INCR', KEYS[1]) 1 s` | -ERR value is not an integer or out of range | -Invalid argument: value is not an integer or out of range | undocumented |
| scripting | helper functions | Kvrocks | differs | `EVAL return redis.status_reply('FINE') 0` | +FINE | "FINE" | undocumented |
| scripting | helper functions | Kvrocks | differs | `EVAL return type(redis.REDIS_VERSION) 0` | "string" | "nil" | undocumented |
| scripting | script kill with no script running | Kvrocks | differs | `SCRIPT KILL` | -NOTBUSY No scripts in execution right now. | -ERR Unknown SCRIPT subcommand or wrong number of arguments | documented: [SCRIPT KILL is marked unsupported](https://kvrocks.apache.org/docs/supported-commands) |
| scripting | function list and fcall | TidesKV | unsupported | `FUNCTION LIST` | [] | -ERR unknown command 'FUNCTION', with args beginning with: 'LIST'  | undocumented |
| server/info | dbsize and time shape | TidesKV | unsupported | `TIME` | ["1794848926", "376675"] | -ERR unknown command 'TIME', with args beginning with:  | undocumented |
| server/info | config get databases | TidesKV | differs | `CONFIG GET databases` | ["databases", "16"] | [] | undocumented |
| server/info | config get databases | Kvrocks | differs | `CONFIG GET databases` | ["databases", "16"] | [] | undocumented |
| server/info | client info | TidesKV | unsupported | `CLIENT INFO` | "id=147 addr=172.20.0.4:35994 laddr=172.20.0.3:6379 fd=8 name= age=0 i... | -ERR unknown subcommand or wrong number of arguments for 'INFO'. Try C... | undocumented |
| server/info | client list | TidesKV | unsupported | `CLIENT LIST` | "id=148 addr=172.20.0.4:36010 laddr=172.20.0.3:6379 fd=8 name= age=0 i... | -ERR unknown subcommand or wrong number of arguments for 'LIST'. Try C... | documented: [CLIENT LIST is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| server/info | hello 2 | Kvrocks | differs | `HELLO 2` | ["server", "redis", "version", "7.2.5", "proto", :2, "id", :150, "mode... | ["server", "redis", "version", "7.0.0", "proto", :2, "mode", "standalo... | undocumented |
| server/info | role | Kvrocks | differs | `ROLE` | ["master", :0, []] | ["master", "721"] | undocumented |
| server/info | command count and info | TidesKV | differs | `COMMAND INFO get` | [["get", :2, [+readonly, +fast], :1, :1, :1, [+@read, +@string, +@fast... | [["get", :2, [+readonly, +fast], :1, :1, :1]] | undocumented |
| server/info | command count and info | Kvrocks | differs | `COMMAND INFO get` | [["get", :2, [+readonly, +fast], :1, :1, :1, [+@read, +@string, +@fast... | [["get", :2, ["readonly"], :1, :1, :1]] | undocumented |
| server/info | command docs | TidesKV | differs | `COMMAND DOCS get` | ["get", ["summary", "Returns the string value of a key.", "since", "1.... | [] | undocumented |
| server/info | command docs | Kvrocks | unsupported | `COMMAND DOCS get` | ["get", ["summary", "Returns the string value of a key.", "since", "1.... | -ERR Command subcommand must be one of COUNT, GETKEYS, INFO | undocumented |
| server/info | slowlog | TidesKV | unsupported | `SLOWLOG LEN` | :0 | -ERR unknown command 'SLOWLOG', with args beginning with: 'LEN'  | undocumented |
| server/info | slowlog | TidesKV | unsupported | `SLOWLOG RESET` | +OK | -ERR unknown command 'SLOWLOG', with args beginning with: 'RESET'  | undocumented |
| server/info | slowlog | TidesKV | unsupported | `SLOWLOG GET 1` | [] | -ERR unknown command 'SLOWLOG', with args beginning with: 'GET' '1'  | undocumented |
| server/info | latency and memory | TidesKV | unsupported | `MEMORY USAGE missing` | (nil) | -ERR unknown command 'MEMORY', with args beginning with: 'USAGE' 'miss... | undocumented |
| server/info | latency and memory | TidesKV | unsupported | `MEMORY USAGE nosuch` | (nil) | -ERR unknown command 'MEMORY', with args beginning with: 'USAGE' 'nosu... | undocumented |
| server/info | latency and memory | TidesKV | unsupported | `LATENCY LATEST` | [] | -ERR unknown command 'LATENCY', with args beginning with: 'LATEST'  | undocumented |
| server/info | latency and memory | Kvrocks | unsupported | `LATENCY LATEST` | [] | -ERR Unknown LATENCY subcommand or wrong number of arguments | undocumented |
| server/info | auth without password | Kvrocks | differs | `AUTH user secret` | -WRONGPASS invalid username-password pair or user is disabled. | -ERR wrong number of arguments | undocumented |
| server/info | wait | Kvrocks | differs | `WAIT 0 0` | :0 | -ERR numreplicas should be a positive integer | undocumented |
| server/info | cluster x commands outside cluster mode | TidesKV | extra | `CLUSTERX VERSION` | -ERR unknown command 'CLUSTERX', with args beginning with: 'VERSION'  | -ERR Cluster mode is not enabled | not a difference (Redis 7.2 lacks it) |
| server/info | cluster x commands outside cluster mode | Kvrocks | extra | `CLUSTERX VERSION` | -ERR unknown command 'CLUSTERX', with args beginning with: 'VERSION'  | -ERR Cluster mode is not enabled | not a difference (Redis 7.2 lacks it) |
| server/info | lastsave and bgsave | TidesKV | unsupported | `LASTSAVE` | :1794848922 | -ERR unknown command 'LASTSAVE', with args beginning with:  | undocumented |
| server/info | lastsave and bgsave | TidesKV | unsupported | `BGSAVE` | +Background saving started | -ERR unknown command 'BGSAVE', with args beginning with:  | undocumented |
| server/info | flushall flushdb async | Kvrocks | differs | `FLUSHDB ASYNC` | +OK | -ERR wrong number of arguments | undocumented |
| server/info | flushall flushdb async | Kvrocks | differs | `FLUSHALL SYNC` | +OK | -ERR wrong number of arguments | undocumented |
| streams | xadd xlen | TidesKV | unsupported | `XADD s 1-1 a 1` | "1-1" | -ERR unknown command 'XADD', with args beginning with: 's' '1-1' 'a' '... | documented: [Streams is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| streams | xadd xlen | TidesKV | unsupported | `XLEN s` | :1 | -ERR unknown command 'XLEN', with args beginning with: 's'  | documented: [Streams is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| streams | xadd xlen | TidesKV | unsupported | `XRANGE s - +` | [["1-1", ["a", "1"]]] | -ERR unknown command 'XRANGE', with args beginning with: 's' '-' '+'  | documented: [Streams is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| bitmaps | setbit getbit bitcount | TidesKV | unsupported | `SETBIT b 7 1` | :0 | -ERR unknown command 'SETBIT', with args beginning with: 'b' '7' '1'  | documented: [Bitmaps is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| bitmaps | setbit getbit bitcount | TidesKV | unsupported | `GETBIT b 7` | :1 | -ERR unknown command 'GETBIT', with args beginning with: 'b' '7'  | documented: [Bitmaps is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| bitmaps | setbit getbit bitcount | TidesKV | unsupported | `BITCOUNT b` | :1 | -ERR unknown command 'BITCOUNT', with args beginning with: 'b'  | documented: [Bitmaps is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| hyperloglog | pfadd pfcount | TidesKV | unsupported | `PFADD h a b c` | :1 | -ERR unknown command 'PFADD', with args beginning with: 'h' 'a' 'b' 'c... | documented: [HyperLogLog is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| hyperloglog | pfadd pfcount | TidesKV | unsupported | `PFCOUNT h` | :3 | -ERR unknown command 'PFCOUNT', with args beginning with: 'h'  | documented: [HyperLogLog is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| geo | geoadd geodist | TidesKV | unsupported | `GEOADD g 13.361389 38.115556 Palermo 15.087269 37....` | :2 | -ERR unknown command 'GEOADD', with args beginning with: 'g' '13.36138... | documented: [Geo is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| geo | geoadd geodist | TidesKV | unsupported | `GEODIST g Palermo Catania km` | "166.2742" | -ERR unknown command 'GEODIST', with args beginning with: 'g' 'Palermo... | documented: [Geo is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| geo | geoadd geodist | Kvrocks | differs | `GEODIST g Palermo Catania km` | "166.2742" | "166.27415156960032" | undocumented |
| pubsub | publish | TidesKV | unsupported | `PUBLISH ch hello` | :0 | -ERR unknown command 'PUBLISH', with args beginning with: 'ch' 'hello'... | documented: [Pub/Sub is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| pubsub | publish | TidesKV | unsupported | `PUBSUB NUMSUB ch` | ["ch", :0] | -ERR unknown command 'PUBSUB', with args beginning with: 'NUMSUB' 'ch'... | documented: [Pub/Sub is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| json | json set get | Kvrocks | extra | `JSON.SET k $ {"a":1}` | -ERR unknown command 'JSON.SET', with args beginning with: 'k' '$' '{"... | +OK | not a difference (Redis 7.2 lacks it) |
| json | json set get | Kvrocks | extra | `JSON.GET k` | -ERR unknown command 'JSON.GET', with args beginning with: 'k'  | "{"a":1}" | not a difference (Redis 7.2 lacks it) |
| search | ft info | Kvrocks | extra | `FT.INFO idx` | -ERR unknown command 'FT.INFO', with args beginning with: 'idx'  | -ERR index not found | not a difference (Redis 7.2 lacks it) |
| bloom | bf add | Kvrocks | extra | `BF.ADD bf a` | -ERR unknown command 'BF.ADD', with args beginning with: 'bf' 'a'  | :1 | not a difference (Redis 7.2 lacks it) |
| bloom | bf add | Kvrocks | extra | `BF.EXISTS bf a` | -ERR unknown command 'BF.EXISTS', with args beginning with: 'bf' 'a'  | :1 | not a difference (Redis 7.2 lacks it) |
| notifications | keyspace events config | TidesKV | differs | `CONFIG SET notify-keyspace-events KEA` | +OK | -ERR CONFIG SET failed (possibly related to argument 'notify-keyspace-... | undocumented |
| notifications | keyspace events config | TidesKV | differs | `CONFIG GET notify-keyspace-events` | ["notify-keyspace-events", "AKE"] | [] | undocumented |
| notifications | keyspace events config | Kvrocks | differs | `CONFIG SET notify-keyspace-events KEA` | +OK | -ERR CONFIG SET 'notify-keyspace-events' error: Unsupported CONFIG par... | documented: [keyspace notifications are a design proposal, not built](https://github.com/apache/kvrocks/discussions/3533) |
| notifications | keyspace events config | Kvrocks | differs | `CONFIG GET notify-keyspace-events` | ["notify-keyspace-events", "AKE"] | [] | documented: [keyspace notifications are a design proposal, not built](https://github.com/apache/kvrocks/discussions/3533) |
| tracking | client tracking on | TidesKV | unsupported | `CLIENT TRACKING ON` | +OK | -ERR unknown subcommand or wrong number of arguments for 'TRACKING'. T... | undocumented |
| tracking | client tracking on | TidesKV | unsupported | `CLIENT TRACKING OFF` | +OK | -ERR unknown subcommand or wrong number of arguments for 'TRACKING'. T... | undocumented |
| tracking | client tracking on | Kvrocks | unsupported | `CLIENT TRACKING ON` | +OK | -ERR Syntax error, try CLIENT LIST\|INFO\|KILL ip:port\|GETNAME\|SETNA... | documented: [CLIENT TRACKING is marked unsupported](https://kvrocks.apache.org/docs/supported-commands) |
| tracking | client tracking on | Kvrocks | unsupported | `CLIENT TRACKING OFF` | +OK | -ERR Syntax error, try CLIENT LIST\|INFO\|KILL ip:port\|GETNAME\|SETNA... | documented: [CLIENT TRACKING is marked unsupported](https://kvrocks.apache.org/docs/supported-commands) |
| object | object encoding | TidesKV | unsupported | `OBJECT ENCODING a` | "int" | -ERR unknown command 'OBJECT', with args beginning with: 'ENCODING' 'a... | documented: [OBJECT is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| object | object encoding | TidesKV | unsupported | `OBJECT ENCODING l` | "listpack" | -ERR unknown command 'OBJECT', with args beginning with: 'ENCODING' 'l... | documented: [OBJECT is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| object | object encoding | TidesKV | unsupported | `OBJECT ENCODING s` | "intset" | -ERR unknown command 'OBJECT', with args beginning with: 'ENCODING' 's... | documented: [OBJECT is listed as not supported yet](docs/tutorial.md#differences-from-redis) |
| object | object encoding | Kvrocks | unsupported | `OBJECT ENCODING a` | "int" | -ERR object subcommand must be dump | documented: [OBJECT ENCODING, FREQ, IDLETIME and REFCOUNT are unsupported](https://kvrocks.apache.org/docs/supported-commands) |
| object | object encoding | Kvrocks | unsupported | `OBJECT ENCODING l` | "listpack" | -ERR object subcommand must be dump | documented: [OBJECT ENCODING, FREQ, IDLETIME and REFCOUNT are unsupported](https://kvrocks.apache.org/docs/supported-commands) |
| object | object encoding | Kvrocks | unsupported | `OBJECT ENCODING s` | "intset" | -ERR object subcommand must be dump | documented: [OBJECT ENCODING, FREQ, IDLETIME and REFCOUNT are unsupported](https://kvrocks.apache.org/docs/supported-commands) |

<details><summary>Same error code, different text (51 commands; not counted above)</summary>

| Family | Case | Server | Command | Redis 7.2 | Server |
|---|---|---|---|---|---|
| strings | set syntax errors | Kvrocks | `SET a 1 EX 0` | -ERR invalid expire time in 'set' command | -ERR out of numeric range |
| strings | set syntax errors | Kvrocks | `SET a` | -ERR wrong number of arguments for 'set' command | -ERR wrong number of arguments |
| strings | setex psetex setnx | Kvrocks | `SETEX d 0 v` | -ERR invalid expire time in 'setex' command | -ERR invalid expire time |
| strings | mset mget msetnx | Kvrocks | `MSET odd` | -ERR wrong number of arguments for 'mset' command | -ERR wrong number of arguments |
| strings | incr errors | Kvrocks | `INCR s` | -ERR value is not an integer or out of range | -ERR Invalid argument: value is not an integer or out of range |
| strings | incr errors | Kvrocks | `INCR big` | -ERR increment or decrement would overflow | -ERR Invalid argument: increment or decrement would overflow |
| strings | incr errors | Kvrocks | `INCR f` | -ERR value is not an integer or out of range | -ERR Invalid argument: value is not an integer or out of range |
| strings | incrbyfloat | Kvrocks | `INCRBYFLOAT s 1` | -ERR value is not a valid float | -ERR Invalid argument: value is not a number |
| keyspace | scan bad cursor | Kvrocks | `SCAN 0 COUNT 0` | -ERR syntax error | -ERR limit should be a positive integer |
| keyspace | dump restore | TidesKV | `RESTORE b 0 \x00\x05hello\x0b\x00\xe1\x01\x00\x00\...` | -ERR DUMP payload version or checksum are wrong | -ERR unknown command 'RESTORE', with args beginning with: 'b' '0' '\x00\x05hello\x0b\x00\xe1\x01\x00... |
| keyspace | dump restore | Kvrocks | `RESTORE b 0 \x00\x05hello\x0b\x00\xe1\x01\x00\x00\...` | -ERR DUMP payload version or checksum are wrong | -ERR invalid or unsupported rdb version: 12336 |
| expire | expire option errors | Kvrocks | `EXPIRE a 100 NX XX` | -ERR NX and XX, GT or LT options at the same time are not compatible | -ERR wrong number of arguments |
| expire | expire option errors | Kvrocks | `EXPIRE a 100 GT LT` | -ERR GT and LT options at the same time are not compatible | -ERR wrong number of arguments |
| expire | expire option errors | Kvrocks | `EXPIRE a abc` | -ERR value is not an integer or out of range | -ERR not started as an integer |
| hash | hmset hmget | Kvrocks | `HSET h` | -ERR wrong number of arguments for 'hset' command | -ERR wrong number of arguments |
| hash | hincrby hincrbyfloat | Kvrocks | `HINCRBY h s 1` | -ERR hash value is not an integer | -ERR Invalid argument: not started as an integer |
| list | lindex lset | Kvrocks | `LSET l 10 X` | -ERR index out of range | -ERR Invalid argument: index out of range |
| set | sintercard | Kvrocks | `SINTERCARD 0 a` | -ERR numkeys should be greater than 0 | -ERR value is out of range, must be positive |
| zset | zadd errors | Kvrocks | `ZADD z 1` | -ERR wrong number of arguments for 'zadd' command | -ERR wrong number of arguments |
| zset | infinite and float scores | Kvrocks | `ZADD z nan d` | -ERR value is not a valid float | -ERR score is not a valid float |
| transactions | queue error aborts exec | Kvrocks | `BADCOMMAND` | -ERR unknown command 'BADCOMMAND', with args beginning with:  | -ERR unknown command `BADCOMMAND`, with args beginning with:  |
| transactions | queue error aborts exec | Kvrocks | `EXEC` | -EXECABORT Transaction discarded because of previous errors. | -EXECABORT Transaction discarded |
| transactions | wrong arity in queue aborts exec | Kvrocks | `SET a` | -ERR wrong number of arguments for 'set' command | -ERR wrong number of arguments |
| transactions | wrong arity in queue aborts exec | Kvrocks | `EXEC` | -EXECABORT Transaction discarded because of previous errors. | -EXECABORT Transaction discarded |
| scripting | redis call error and pcall | Kvrocks | `EVAL return redis.call('INCR', KEYS[1]) 1 s` | -ERR value is not an integer or out of range script: 61636018f4e6b5817b89791bbed242f93fa089e3, on @u... | -ERR running script (call to f_61636018f4e6b5817b89791bbed242f93fa089e3): Invalid argument: value is... |
| scripting | redis call error and pcall | Kvrocks | `EVAL return redis.call('NOSUCHCMD') 0` | -ERR Unknown Redis command called from script script: ee4002f488cb0514b4fbe5979e61c3bf43204edb, on @... | -ERR running script (call to f_ee4002f488cb0514b4fbe5979e61c3bf43204edb): Unknown Redis command call... |
| scripting | evalsha script load exists flush | Kvrocks | `EVALSHA 1fa00e76656cc152ad327c13fe365858fd7be306 0` | -NOSCRIPT No matching script. Please use EVAL. | -NOSCRIPT No matching script. Please use EVAL |
| scripting | evalsha unknown | Kvrocks | `EVALSHA ffffffffffffffffffffffffffffffffffffffff 0` | -NOSCRIPT No matching script. Please use EVAL. | -NOSCRIPT No matching script. Please use EVAL |
| scripting | eval ro | Kvrocks | `EVAL_RO return redis.call('SET', KEYS[1], 1) 1 k` | -ERR Write commands are not allowed from read-only scripts. script: 897afd4d23ee70efe0ede1685c1c9483... | -ERR running script (call to f_897afd4d23ee70efe0ede1685c1c94834429da77): Write commands are not all... |
| scripting | globals are protected | Kvrocks | `EVAL x = 1; return x 0` | -ERR user_script:1: Attempt to modify a readonly table script: 757ed4623e097b3c66eab1c4eea3bedcab153... | -ERR running script (call to f_757ed4623e097b3c66eab1c4eea3bedcab153cdf): @enable_strict_lua:8: user... |
| scripting | globals are protected | Kvrocks | `EVAL return os.time() 0` | -ERR user_script:1: Script attempted to access nonexistent global variable 'os' script: 13dea82ee9da... | -ERR running script (call to f_13dea82ee9da896aebcd10f5e36f4a1eeb839b48): @enable_strict_lua:15: use... |
| scripting | script syntax error | Kvrocks | `EVAL return + 0` | -ERR Error compiling script (new function): user_script:1: unexpected symbol near '+' | -ERR Error while compiling new script: user_script:1: unexpected symbol near '+' |
| scripting | script syntax error | Kvrocks | `EVAL error('boom') 0` | -ERR user_script:1: boom script: 82903a0434f1503e152f89c03c9acd881a0e8150, on @user_script:1. | -ERR running script (call to f_82903a0434f1503e152f89c03c9acd881a0e8150): @user_script:1: user_scrip... |
| scripting | function list and fcall | TidesKV | `FCALL nosuchfn 0` | -ERR Function not found | -ERR unknown command 'FCALL', with args beginning with: 'nosuchfn' '0'  |
| scripting | function list and fcall | Kvrocks | `FCALL nosuchfn 0` | -ERR Function not found | -ERR No such function name found in storage: NotFound:  |
| scripting | script debug and unknown subcommand | Kvrocks | `SCRIPT NOSUCH` | -ERR unknown subcommand 'NOSUCH'. Try SCRIPT HELP. | -ERR Unknown SCRIPT subcommand or wrong number of arguments |
| server/info | ping echo | Kvrocks | `ECHO` | -ERR wrong number of arguments for 'echo' command | -ERR wrong number of arguments |
| server/info | config set errors | TidesKV | `CONFIG SET nosuchparam 1` | -ERR Unknown option or number of arguments for CONFIG SET - 'nosuchparam' | -ERR CONFIG SET failed (possibly related to argument 'nosuchparam') - unknown option 'nosuchparam' |
| server/info | config set errors | TidesKV | `CONFIG SET maxclients abc` | -ERR CONFIG SET failed (possibly related to argument 'maxclients') - argument couldn't be parsed int... | -ERR CONFIG SET failed (possibly related to argument 'maxclients') - argument must be between 1 and ... |
| server/info | config set errors | Kvrocks | `CONFIG SET nosuchparam 1` | -ERR Unknown option or number of arguments for CONFIG SET - 'nosuchparam' | -ERR CONFIG SET 'nosuchparam' error: Unsupported CONFIG parameter: nosuchparam |
| server/info | config set errors | Kvrocks | `CONFIG SET maxclients abc` | -ERR CONFIG SET failed (possibly related to argument 'maxclients') - argument couldn't be parsed int... | -ERR CONFIG SET 'maxclients' error: failed to set new value: not started as an integer |
| server/info | client unknown subcommand | TidesKV | `CLIENT NOSUCH` | -ERR unknown subcommand 'NOSUCH'. Try CLIENT HELP. | -ERR unknown subcommand or wrong number of arguments for 'NOSUCH'. Try CLIENT HELP. |
| server/info | client unknown subcommand | Kvrocks | `CLIENT NOSUCH` | -ERR unknown subcommand 'NOSUCH'. Try CLIENT HELP. | -ERR Syntax error, try CLIENT LIST\|INFO\|KILL ip:port\|GETNAME\|SETNAME\|SETINFO\|REPLY\|PAUSE\|UNP... |
| server/info | auth without password | Kvrocks | `AUTH secret` | -ERR AUTH <password> called without any password configured for the default user. Are you sure your ... | -ERR Client sent AUTH, but no password is set |
| server/info | cluster commands outside cluster mode | Kvrocks | `CLUSTER INFO` | -ERR This instance has cluster support disabled | -ERR Cluster mode is not enabled |
| server/info | cluster commands outside cluster mode | Kvrocks | `CLUSTER MYID` | -ERR This instance has cluster support disabled | -ERR CLUSTER command, CLUSTER INFO\|NODES\|SLOTS\|KEYSLOT\|RESET\|REPLICAS |
| server/info | debug and lolwut | TidesKV | `DEBUG NOSUCH` | -ERR DEBUG command not allowed. If the enable-debug-command option is set to "local", you can run it... | -ERR unknown subcommand or wrong number of arguments for 'NOSUCH'. Try DEBUG HELP. |
| server/info | debug and lolwut | Kvrocks | `DEBUG NOSUCH` | -ERR DEBUG command not allowed. If the enable-debug-command option is set to "local", you can run it... | -ERR Syntax error, DEBUG SLEEP <seconds>\|PROTOCOL <type>\|DBSIZE-LIMIT <0\|1> |
| server/info | unknown command and arity | Kvrocks | `NOSUCHCOMMAND` | -ERR unknown command 'NOSUCHCOMMAND', with args beginning with:  | -ERR unknown command `NOSUCHCOMMAND`, with args beginning with:  |
| server/info | unknown command and arity | Kvrocks | `GET` | -ERR wrong number of arguments for 'get' command | -ERR wrong number of arguments |
| server/info | unknown command and arity | Kvrocks | `GET a b` | -ERR wrong number of arguments for 'get' command | -ERR wrong number of arguments |

</details>

## F2: operations facts

Facts about operating each server, each with the source it comes from. No scores. Kvrocks facts are from its official
documentation or GitHub repositories; TidesKV facts are from this repository's README and tutorial, named by their paths. **TidesKV is pre-1.0
(version 0.1.0): its interfaces and on-disk format may still change.** "Not verified" means the source was read and does not
settle the point; it does not mean the feature is absent. The Kvrocks pages were read on 2026-11-12.

| Topic | TidesKV (pre-1.0) | Kvrocks |
|---|---|---|
| Backup and restore | No backup or checkpoint command is documented (README (`README.md`), tutorial (`docs/tutorial.md`)). A new replica gets a full sync, a logical snapshot stream (README: Replication (`README.md#replication`)). It cannot read Redis RDB files or a Kvrocks data directory (tutorial: Differences from Redis (`docs/tutorial.md#differences-from-redis`)). | `BGSAVE` makes a backup; copy its files with rsync, fully or incrementally ([backup doc](https://kvrocks.apache.org/docs/backup)). That page does not explain how to restore: not verified. |
| Metrics | `INFO` has the sections server, clients, memory, stats, persistence, replication, cluster and keyspace (tutorial: Observability (`docs/tutorial.md#observability`)). No exporter is documented. | Metrics are exposed through `INFO` ([INFO sections](https://kvrocks.apache.org/docs/info-sections)). A separate project, kvrocks_exporter, exposes them for Prometheus ([exporter doc](https://kvrocks.apache.org/docs/kvrocks-exporter)). |
| Online config | `CONFIG GET` and `CONFIG SET`; the tutorial marks which settings change at runtime, the others need a restart (tutorial: Configuration (`docs/tutorial.md`)). `CONFIG REWRITE` is not documented: not verified. Command support is partial per subcommand: in this run's F1 section TidesKV refused `LATENCY LATEST` and `SLOWLOG`, and answered `CONFIG GET databases` with an empty reply. | `CONFIG GET`, `SET` and `REWRITE` are listed as supported since v1.0.0; `CONFIG RESETSTAT` is not ([supported commands](https://kvrocks.apache.org/docs/supported-commands)). Support is partial per subcommand: in this run's F1 section Kvrocks refused `LATENCY LATEST`, rejected `WAIT 0 0` ("numreplicas should be a positive integer") and answered `CONFIG GET databases` with an empty reply. |
| TLS | Not supported yet, planned for v1.x (README (`README.md`)). | `tls-port`, `tls-cert-file` and `tls-replication` settings exist in `kvrocks.conf` on the `unstable` branch ([kvrocks.conf](https://github.com/apache/kvrocks/blob/unstable/kvrocks.conf)); building with TLS needs OpenSSL ([README](https://github.com/apache/kvrocks)). Whether the 2.17.0 image is built with TLS: not verified. |
| ACL | Not supported yet, planned for v1.x (README (`README.md`)). `AUTH` with `requirepass` is available (README: configuration (`README.md`)). | The supported-commands page has no ACL section and `kvrocks.conf` (`unstable` branch) has no ACL setting ([supported commands](https://kvrocks.apache.org/docs/supported-commands)); passwords and per-namespace tokens are documented ([namespace doc](https://kvrocks.apache.org/docs/namespace)). |
| Cluster controller | [kvrocks-controller](https://github.com/apache/kvrocks-controller) 1.3.0 can manage a TidesKV cluster through the `CLUSTERX` commands; nodes do not gossip (README: Cluster mode (`README.md#cluster-mode`)). | kvrocks-controller handles failover and removal of failed nodes, scales out clusters, and uses etcd as its default store ([controller README](https://github.com/apache/kvrocks-controller)); topology is set through `CLUSTERX` ([cluster doc](https://kvrocks.apache.org/docs/cluster)). |
| CDC hooks | No change-capture tool or hook is documented. Replicas follow an internal replication log (tutorial: Replication (`docs/tutorial.md#replication`)). | kvrocks2redis loads the local data directory and then follows the change stream with `psync` to a Redis or Kvrocks target; it must run on the same machine and flushes the target first ([kvrocks2redis doc](https://kvrocks.apache.org/docs/kvrocks2redis)). |
| Upgrade path | None promised: pre-1.0, the on-disk format may change between versions (tutorial: Differences from Redis (`docs/tutorial.md#differences-from-redis`)). | Each release lists compatibility notes; 2.17.0's cover admin-only commands, slot migration types and client output-buffer limits ([releases](https://github.com/apache/kvrocks/releases)). Rolling-upgrade procedure: not verified. |
| Release history | Version 0.1.0 (CMakeLists.txt (`CMakeLists.txt`), README status (`README.md`)); the repository has no tagged releases. | The releases page lists, newest first, with the day and month it shows: 2.17.0 (19 Sep), 2.16.0 (21 Jun), 2.15.0 (27 Feb), 2.14.0 (11 Nov), 2.13.0 (05 Aug), 2.12.1 (06 May), 2.12.0 (date not shown), 2.11.0 (26 Jan; a 2.11.1-rc1 is named only in a compare link), 2.10.1 (24 Oct). 2.17.0 is the version benchmarked here ([releases](https://github.com/apache/kvrocks/releases)). The page shows no year: not verified. |
| Named production users | None published (README (`README.md`)). | The project's users page lists 26 names, with no workload detail: Ahoy Games, Baidu, BaiShanCloud, Biqutex, CIRCL, CoinIndex, CTrip, CVAT, Datavisor, Gcore, Huolala, iFlytek ADX, iQIYI, Jiatou, Meitu, Netease Hangzhou Institute, Opera, RAGFlow, Ruangao Mobile Private Marketplace, Shoplazza, TongCheng, U-NEXT, WakaTime, Xueqiu, Youdao, ZTO Express ([users](https://kvrocks.apache.org/users/)). |

## Methodology

1. **Repetitions.** Every timed cell ran 1 time(s), servers interleaved per repetition (TidesKV, then each Kvrocks configuration, then again). The tables give the median and the min-max range. This is a quick run: one repetition, so no verdicts are drawn.
2. **Verdict rule.** The ratio is oriented so that above 1 means TidesKV is better (ops/s: TidesKV over Kvrocks; latency, bytes, recovery time: Kvrocks over TidesKV). A result is a tie when the ratio is inside the larger of the two sides' relative spread (max-min over median) or 5%. Verdicts are per metric, against the Kvrocks configuration that is best for that metric, so TidesKV is never compared with a weaker Kvrocks setting. A case row says "mixed" when its metrics disagree; there is no weighting. A metric with an error on either side (a failed repetition, error replies) gets no verdict.
3. **Open-loop latency.** For U1 and U3 the closed-loop run finds each server's maximum, then both run at 25%, 50% and 75% of the lower server's maximum (memtier per-connection rate limit), so neither is measured saturated. Latency is only given a verdict from these equal-load cells, and withheld when a server did not reach 95% of the offered rate ("under target"). U2 churn p99.9 also gets a verdict: it runs at a fixed rate that is identical for both servers, and it is withheld when under target. Closed-loop latency is shown, never compared.
4. **Equal resources.** Server on CPUs 0-3 (4 logical CPUs on 2 physical cores), memory cap 8g, open files 65536; client on CPUs 8-15. Workers 4; block cache 2048 MB (TidesKV 2147483648 bytes); memtable 256 MB (TidesKV 268435456 bytes); compression lz4. Settings are read back from the running server before any cell is measured (a mismatch fails the cell).
5. **Durability.** U1-U5 and U8 run relaxed (TidesKV `sync-mode none`, Kvrocks `write_options.sync no`); relaxed is not power-loss safe on either side. U6 and U7 run strict (fsync before every acknowledged write on both sides). TidesKV's interval mode is not compared with Kvrocks `sync yes`. Strict mode is configured and read back, but per-ack fsync is not observed by the suite (neither server reports an fsync count).
6. **Data.** LZ4 is on for both servers. Loads and U3's aging write values that compress to about half (random bytes followed by a copy of them, as RocksDB's db_bench does), the same bytes on both servers (`uc_loader.py`). memtier's own values are not used for any footprint: they are one buffer with one byte changed per request, so a block of them compresses to ~15% while a single value does not compress at all, which favours a block compressor (Kvrocks) and penalises a per-value one. The timed phases still write memtier values, so the keys they overwrite change payload; U8's load speed is a memtier load. TidesKV does not compress values of 1 KiB and up; Kvrocks compresses only its bottom level by default, and the share of its SST bytes left uncompressed is recorded next to each of its disk figures. No observed on-disk / logical ratio is available in this run. Disk figures are allocated bytes (`du --block-size=1`) after the server went quiet, excluding Kvrocks' WAL archive (default `wal_ttl_seconds 10800`), which is reported on its own line; apparent size is shown beside them. Runs recorded before the allocated column existed fall back to apparent size, and the tables label it as such.
7. **Correctness.** Checked per cell from memtier's per-command byte and hit counts: every SET reply must be the 5-byte `+OK` (U1, U2, U3, U5, U6), every HSET and HDEL reply a 4-byte integer (U4), and GETs must hit at least 99% after a load (U1, U3, U5). A failed check makes the cell ERR, shown as such and not as a number. Not checked: U2's GET replies (nearly all are negative lookups by design; the hit rate is reported) and U4's HGET and HGETALL replies (memtier counts no hits for them and their lengths vary). U7's writer stops on an error reply, and its verify step reads back every acknowledged value.
8. **Client not the bottleneck.** A cell whose memtier used over 85% of the client CPUs is flagged client-bound and gets no verdict.
9. **Baselines and host.** Host: 12th Gen Intel(R) Core(TM) i9-12900HK, kernel 6.8.0-147-generic. Loopback networking inside one Docker network.
   - fio 256 KiB random reads (O_DIRECT) on the benchmark filesystem: QD1 889 MB/s, 3392 IOPS; QD2 1735 MB/s, 6620 IOPS; QD4 2963 MB/s, 11302 IOPS; QD8 4250 MB/s, 16212 IOPS; QD16 4880 MB/s, 18615 IOPS; QD32 5065 MB/s, 19322 IOPS.
   - durable-append floor (4 KiB sequential write + fdatasync, queue depth 1; the U6 floor) at the start of the run: 1621 writes/s, flush latency mean 0.603 ms, p99 1.843 ms.
   - fio reference (4 KiB random write + fsync, queue depth 1) at the start of the run: 1498 writes/s, flush latency mean 0.652 ms, p99 2.212 ms.
10. **Raw data.** memtier JSON, INFO before and after each cell, configs as read back, the du series and the commit and image digests are in the run directory (TidesKV 5db560b; apache/kvrocks@sha256:a1e99b3653fb900d5a17b80a9487f74079edc6d5e9b70e3103c01cbfc0d5146a; redislabs/memtier_benchmark:2.1.4).

### Settings on each side

Both servers get the shared parity configuration plus the case's documented choice. Every setting that was written, as read back:

**Relaxed** (U1, U2, U3, U4, U5, U8)

- kvrocks: `rocksdb.block_cache_size 2048`, `rocksdb.compression lz4`, `rocksdb.write_buffer_size 256`, `rocksdb.write_options.sync no`, `workers 4`
- kvrocks-blob: `rocksdb.block_cache_size 2048`, `rocksdb.compression lz4`, `rocksdb.enable_blob_files yes`, `rocksdb.min_blob_size 4096`, `rocksdb.write_buffer_size 256`, `rocksdb.write_options.sync no`, `workers 4`
- kvrocks-cron: `compaction-checker-cron * * * * *`, `rocksdb.block_cache_size 2048`, `rocksdb.compression lz4`, `rocksdb.write_buffer_size 256`, `rocksdb.write_options.sync no`, `workers 4`
- tideskv: `block-cache-size 2147483648`, `compression lz4`, `memtable-size 268435456`, `sync-mode none`, `workers 4`

**Strict** (U6, U7)

- kvrocks: `rocksdb.block_cache_size 2048`, `rocksdb.compression lz4`, `rocksdb.write_buffer_size 256`, `rocksdb.write_options.sync yes`, `workers 4`
- tideskv: `block-cache-size 2147483648`, `compression lz4`, `memtable-size 268435456`, `sync-mode full`, `workers 4`

### Not covered in v1

- Replication, failover and slot migration under load (needs a multi-node harness).
- Memory sweeps from 1x to 10x RAM, relaxed-mode pipeline and connection sweeps, scans after deletes.
- Power-loss simulation: U7 kills the server process, it does not cut power or crash the OS.
- MULTI/Lua atomicity across a crash.
- Kvrocks features TidesKV does not have are listed in F1 and F2, not benchmarked.
- Any change to either server's code or any tuning beyond the settings listed above.

### For Kvrocks maintainers

This comparison is meant to be fair, and the people who know Kvrocks best can tell where it is not. If a setting above is not what you would run in production, a workload is shaped in a way that favours one side, or a number looks wrong, please say so: every setting, command line and raw result needed to reproduce a number is in the run directory, and a corrected configuration will be re-run and published next to the original.
